// Copyright (c) 2022-2024. NetFoundry Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// You may obtain a copy of the License at
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "zt_internal.h"
#include "utils.h"

#define BRIDGE_MSG_SIZE (32 * 1024)
#define BRIDGE_POOL_SIZE 16

#define BR_LOG(lvl, fmt, ...) ZITI_LOG(lvl, "br[%d.%d] " fmt, \
br ? br->conn->ziti_ctx->id : -1, br ? br->conn->conn_id : -1, ##__VA_ARGS__)

struct fd_bridge_s {
    uv_file in;
    uv_file out;

    void (*close_cb)(void *ctx);

    void *ctx;
};

struct ziti_bridge_s {
    bool closed;
    bool ziti_eof;
    bool input_eof;
    ziti_connection conn;
    uv_handle_t *input;
    uv_handle_t *output;
    uv_close_cb close_cb;
    void *data;
    struct fd_bridge_s *fdbr;
    pool_t *input_pool;
    bool input_throttle;
    unsigned long idle_timeout;
    deadline_t idler;
};

static ssize_t on_ziti_data(ziti_connection conn, const uint8_t *data, ssize_t len);

static void bridge_alloc(uv_handle_t *h, size_t req, uv_buf_t *b);
static void close_bridge(struct ziti_bridge_s *br);

static void on_input(uv_stream_t *s, ssize_t len, const uv_buf_t *b);
static void on_udp_input(uv_udp_t *udp, ssize_t len, const uv_buf_t *b, const struct sockaddr *addr, unsigned int flags);
static int fmt_addr(struct sockaddr_storage *ss, char *host, size_t host_len, int *port);


extern int ziti_conn_bridge(ziti_connection conn, uv_handle_t *handle, uv_close_cb on_close) {
    if (handle == NULL || conn == NULL) return UV_EINVAL;

    if ( !(handle->type == UV_TCP || handle->type == UV_NAMED_PIPE ||
           handle->type == UV_TTY || handle->type == UV_UDP )) {
        return UV_EINVAL;
    }

    if (handle->type == UV_UDP) {
        struct sockaddr_storage peer;
        int len = sizeof(peer);
        int rc = uv_udp_getpeername((const uv_udp_t *) handle, (struct sockaddr *) &peer, &len);
        if (rc != 0) {
            ZITI_LOG(ERROR, "cannot bridge unconnected socket: %d/%s", rc, uv_strerror(rc));
            return UV_EINVAL;
        }
    }

    int rc;
    if ((rc = ziti_conn_set_data_cb(conn, on_ziti_data)) != ZITI_OK) {
        ZITI_LOG(ERROR, "failed to bridge ziti connection: %s", ziti_errorstr(rc));
        return UV_ECONNRESET;
    }

    NEWP(br, struct ziti_bridge_s);
    br->conn = conn;
    br->input = handle;
    br->output = handle;
    br->close_cb = on_close;
    br->data = uv_handle_get_data(handle);
    br->input_pool = pool_new(BRIDGE_MSG_SIZE, BRIDGE_POOL_SIZE, NULL);

    uv_handle_set_data(handle, br);
    ziti_conn_set_data(conn, br);
    conn->bridged = true;

    rc = (br->input->type == UV_UDP) ?
         uv_udp_recv_start((uv_udp_t *) br->input, bridge_alloc, on_udp_input) :
         uv_read_start((uv_stream_t *) br->input, bridge_alloc, on_input);

    if (rc != 0) {
        BR_LOG(WARN, "failed to start reading handle: %d/%s", rc, uv_strerror(rc));
        close_bridge(br);
        return UV_ECONNABORTED;
    } else {
        BR_LOG(DEBUG, "connected");
    }

    return ZITI_OK;
}

static void on_sock_close(uv_handle_t *h) {
    struct fd_bridge_s *fdbr = h->data;
    if (fdbr) {
        if (fdbr->close_cb) {
            fdbr->close_cb(fdbr->ctx);
        }
        free(fdbr);
    }
    uv_close(h, (uv_close_cb) free);
}

static void on_pipes_close(uv_handle_t *h) {
    struct ziti_bridge_s *br = h->data;
    uv_close((uv_handle_t *) br->input, (uv_close_cb) free);
    uv_close((uv_handle_t *) br->output, (uv_close_cb) free);
    if (br->fdbr) {
        if (br->fdbr->close_cb) {
            br->fdbr->close_cb(br->fdbr->ctx);
        }
        free(br->fdbr);
    }
}

extern int ziti_conn_bridge_fds(ziti_connection conn, uv_file input, uv_file output, void (*close_cb)(void *ctx), void *ctx) {
    if (conn == NULL) return UV_EINVAL;

    uv_loop_t *l = ziti_conn_context(conn)->loop;

    NEWP(fdbr, struct fd_bridge_s);
    fdbr->in = input;
    fdbr->out = output;
    fdbr->close_cb = close_cb;
    fdbr->ctx = ctx;

    if (input == output) {
        uv_handle_t *sock = NULL;
        int type;
        socklen_t len = sizeof(type);
        if (getsockopt(input, SOL_SOCKET, SO_TYPE, (void *) &type, &len) == 0) {
            if (type == SOCK_STREAM) {
                sock = calloc(1, sizeof(uv_tcp_t));
                uv_tcp_init(l, (uv_tcp_t *) sock);
                uv_tcp_open((uv_tcp_t *) sock, input);
            } else if (type == SOCK_DGRAM) {
                sock = calloc(1, sizeof(uv_udp_t));
                uv_udp_init(l, (uv_udp_t *) sock);
                uv_udp_open((uv_udp_t *) sock, input);
            }
        }
        if (sock) {
            sock->data = fdbr;
        } else {
            ZITI_LOG(ERROR, "unsupported fd type");
            return UV_EINVAL;
        }

        return ziti_conn_bridge(conn, sock, on_sock_close);
    }

    NEWP(br, struct ziti_bridge_s);
    br->conn = conn;
    br->input = calloc(1, sizeof(uv_pipe_t));
    br->output = calloc(1, sizeof(uv_pipe_t));
    br->input_pool = pool_new(BRIDGE_MSG_SIZE, BRIDGE_POOL_SIZE, NULL);

    uv_pipe_init(l, (uv_pipe_t *) br->input, 0);
    uv_pipe_init(l, (uv_pipe_t *) br->output, 0);
    uv_pipe_open((uv_pipe_t *) br->input, input);
    uv_pipe_open((uv_pipe_t *) br->output, output);
    br->input->data = br;
    br->output->data = br;

    br->close_cb = on_pipes_close;

    br->data = br;
    br->fdbr = fdbr;

    uv_handle_set_data(br->input, br);
    ziti_conn_set_data(conn, br);
    conn->bridged = true;

    ziti_conn_set_data_cb(conn, on_ziti_data);
    int rc = uv_read_start((uv_stream_t *) br->input, bridge_alloc, on_input);
    if (rc != 0) {
        BR_LOG(WARN, "failed to start reading handle: %d/%s", rc, uv_strerror(rc));
        close_bridge(br);
    } else {
        BR_LOG(DEBUG, "connected");
    }
    return ZITI_OK;
}

static void on_bridge_idle(void *v) {
    struct ziti_bridge_s *br = v;
    BR_LOG(DEBUG, "closing bridge due to idle timeout");
    close_bridge(br);
}

static void br_set_idle_timeout(struct ziti_bridge_s *br) {
    if (br->idle_timeout > 0) { // reset idle timer
        ztx_set_deadline(br->conn->ziti_ctx, br->idle_timeout, &br->idler, on_bridge_idle, br);
    }
}

int ziti_conn_bridge_idle_timeout(ziti_connection conn, unsigned long millis) {
    struct ziti_bridge_s *br = ziti_conn_data(conn);
    if (millis == 0) {
        br->idle_timeout = 0;
    } else {
        br->idle_timeout = millis;
        br_set_idle_timeout(br);
    }
    return 0;
}

static void on_ziti_close(ziti_connection conn) {
    struct ziti_bridge_s *br = ziti_conn_data(conn);
    pool_destroy(br->input_pool);
    free(br);
}

static void close_bridge(struct ziti_bridge_s *br) {
    if (br == NULL || br->closed) { return; }

    BR_LOG(DEBUG, "closing");
    br->closed = true;

    if (br->input) {
        uv_handle_set_data((uv_handle_t *) br->input, br->data);
        br->close_cb((uv_handle_t *) br->input);
        br->input = NULL;
    }

    ziti_close(br->conn, on_ziti_close);
}

static void on_shutdown(uv_shutdown_t *sr, int status) {
    // ignore UV_ECANCELED, it just means that stream was closed
    // before shutdown was processed
    if (status != 0 && status != UV_ECANCELED) {
        struct ziti_bridge_s *br = sr->handle->data;
        BR_LOG(WARN, "shutdown failed: %d(%s)", status, uv_strerror(status));
        close_bridge(sr->handle->data);
    }
    free(sr);
}

ssize_t on_ziti_data(ziti_connection conn, const uint8_t *data, ssize_t len) {
    struct ziti_bridge_s *br = ziti_conn_data(conn);

    if (br == NULL) {
        ziti_close(conn, NULL);
        return -1;
    }

    br_set_idle_timeout(br);

    if (len > 0) {
        BR_LOG(TRACE, "received %zd bytes from ziti", len);
        uv_buf_t b = uv_buf_init((char *) data, len);

        ssize_t rc = br->output->type == UV_UDP ?
                     uv_udp_try_send((uv_udp_t *) br->output, &b, 1, NULL) :
                     uv_try_write((uv_stream_t *) br->output, &b, 1);

        if (rc >= 0) {
            return rc;
        }
        else if (rc == UV_EAGAIN) { // EWOULDBLOCK
            return 0;
        }
        else {
            BR_LOG(WARN, "write failed: %zd(%s)", rc, uv_strerror((int) rc));
            close_bridge(br);
            return rc;
        }

    } else if (len == ZITI_EOF) {
        BR_LOG(VERBOSE, "received EOF from ziti");
        br->ziti_eof = true;
        if (br->input_eof || br->input->type == UV_UDP) {
            BR_LOG(VERBOSE, "both sides are EOF");
            close_bridge(br);
        }
        else {
            NEWP(sr, uv_shutdown_t);
            int rc = uv_shutdown(sr, (uv_stream_t *) br->output, on_shutdown);
            if (rc != 0) {
                free(sr);
                BR_LOG(WARN, "shutdown failed: %d/%s", rc, uv_strerror(rc));
                close_bridge(br);
            }
        }
    } else {
        if (len == ZITI_CONN_CLOSED) {
            BR_LOG(VERBOSE, "closing bridge");
        } else {
            BR_LOG(WARN, "closing bridge due to error: %zd(%s)", len, ziti_errorstr((int) len));
        }
        close_bridge(br);
    }
    return 0;
}

void bridge_alloc(uv_handle_t *h, size_t req, uv_buf_t *b) {
    struct ziti_bridge_s *br = h->data;

    BR_LOG(TRACE, "alloc %s", br->input_throttle ? "stalled" : "live");

    b->base = pool_alloc_obj(br->input_pool);
    b->len = pool_obj_size(b->base);
    if (b->base != NULL) {
        if (br->input_throttle) {
            BR_LOG(TRACE, "unstalled");
        }
        br->input_throttle = false;
    }
}

static void on_ziti_write(ziti_connection conn, ssize_t status, void *ctx) {
    pool_return_obj(ctx);
    struct ziti_bridge_s *br = ziti_conn_data(conn);

    if (status < ZITI_OK) {
        BR_LOG(DEBUG, "ziti_write failed: %zd/%s", status, ziti_errorstr(status));
        close_bridge(br);
    }
    else if (br->input) {
        if (br->input_throttle) {
            br->input_throttle = false;
            int rc = br->input->type == UV_UDP ?
                     uv_udp_recv_start((uv_udp_t *) br->input, bridge_alloc, on_udp_input) :
                     uv_read_start((uv_stream_t *) br->input, bridge_alloc, on_input);

            if (rc != 0) {
                BR_LOG(WARN, "failed to start reading handle: %d/%s", rc, uv_strerror(rc));
                close_bridge(br);
            } else {
                BR_LOG(DEBUG, "connected");
            }
        }
    }
}

void on_udp_input(uv_udp_t *udp, ssize_t len, const uv_buf_t *b, const struct sockaddr *addr, unsigned int flags) {
    struct ziti_bridge_s *br = udp->data;

    br_set_idle_timeout(br);

    if (len > 0) {
        int rc = ziti_write(br->conn, (uint8_t *) b->base, len, on_ziti_write, b->base);
        if (rc != ZITI_OK) {
            BR_LOG(WARN, "ziti_write failed: %d/%s", rc, ziti_errorstr(rc));
            close_bridge(br);
        }
    } else {
        pool_return_obj(b->base);
        if (len == UV_ENOBUFS) {
            if (!br->input_throttle) {
                BR_LOG(TRACE, "stalled");
                br->input_throttle = true;
                uv_udp_recv_stop(udp);
            }
        } else if (len < 0) {
            BR_LOG(WARN, "err = %zd/%s", len, uv_strerror(len));
            close_bridge(br);
        }
    }
}

void on_input(uv_stream_t *s, ssize_t len, const uv_buf_t *b) {
    struct ziti_bridge_s *br = s->data;

    br_set_idle_timeout(br);

    if (len > 0) {
        int rc = ziti_write(br->conn, (uint8_t *) b->base, len, on_ziti_write, b->base);
        if (rc != ZITI_OK) {
            BR_LOG(WARN, "ziti_write failed: %d/%s", rc, ziti_errorstr(rc));
            close_bridge(br);
        }
    } else {
        pool_return_obj(b->base);
        if (len == UV_ENOBUFS) {
            if (!br->input_throttle) {
                BR_LOG(TRACE, "stalled");
                br->input_throttle = true;
                uv_read_stop(s);
            }
        } else if (len == UV_EOF) {
            br->input_eof = true;
            if (br->ziti_eof) {
                BR_LOG(VERBOSE, "both sides are EOF");
                close_bridge(br);
            } else {
                ziti_close_write(br->conn);
            }
        } else if (len < 0) {
            BR_LOG(WARN, "err = %zd", len);
            close_bridge(br);
        }
    }
}

int conn_bridge_info(ziti_connection conn, char *buf, size_t buflen) {
    if (conn == NULL || !conn->bridged) {
        return ZITI_INVALID_STATE;
    }

    struct ziti_bridge_s *br = conn->data;
    const char *proto = NULL;
    struct sockaddr_storage local;
    int local_len = sizeof(local);
    struct sockaddr_storage remote;
    int remote_len = sizeof(remote);
    int lport = 0;
    int rport = 0;
    char remote_str[128] = "unknown";
    char local_str[128] = "unknown";

    switch (br->output->type) {
        case UV_NAMED_PIPE: {
            char name[256];
            size_t name_size = sizeof(name);
            if (uv_pipe_getpeername((const uv_pipe_t *) br->output, name, &name_size) == 0) {
                snprintf(buf, buflen, "pipe: -> %.*s", (int)name_size, name);
                return ZITI_OK;
            } else {
                return ZITI_INVALID_STATE;
            }
        }
        case UV_TCP:
            proto = "tcp";
            uv_tcp_getsockname((const uv_tcp_t *) br->output, (struct sockaddr *) &local, &local_len);
            uv_tcp_getpeername((const uv_tcp_t *) br->output, (struct sockaddr *) &remote, &remote_len);
            break;
        case UV_UDP:
            uv_udp_getsockname((const uv_udp_t *) br->output, (struct sockaddr *) &local, &local_len);
            uv_udp_getpeername((const uv_udp_t *) br->output, (struct sockaddr *) &remote, &remote_len);
            proto = "udp";
            break;
        default:
            return ZITI_INVALID_STATE;
    }

    fmt_addr(&local, local_str, sizeof(local), &lport);
    fmt_addr(&remote, remote_str, sizeof(remote_str), &rport);

    snprintf(buf, buflen, "%s: %s:%d -> %s:%d", proto, local_str, lport, remote_str, rport);
    return ZITI_OK;
}

static int fmt_addr(struct sockaddr_storage *ss, char *host, size_t host_len, int *port) {
    void *addr = NULL;
    if (ss->ss_family == AF_INET) {
        struct sockaddr_in *in_addr = (struct sockaddr_in *) ss;
        *port = ntohs(in_addr->sin_port);
        addr = &in_addr->sin_addr;
    } else if (ss->ss_family == AF_INET6) {
        struct sockaddr_in6 *in_addr = (struct sockaddr_in6 *) ss;
        *port = ntohs(in_addr->sin6_port);
        addr = &in_addr->sin6_addr;
    } else {
        *port = -1;
        return ZITI_INVALID_STATE;
    }

    uv_inet_ntop(ss->ss_family, addr, host, host_len);
    return ZITI_OK;
}
