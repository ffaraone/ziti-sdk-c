// Copyright (c) 2022-2023.  NetFoundry Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ziti/ziti_src.h>
#include <ziti/ziti_log.h>
#include <string.h>

/**
 * Inherits from uv_lint_t and used to register as source link for `um_http`,
 * sening HTTP traffic over a Ziti connection.
 */
typedef struct ziti_link_s {
    UV_LINK_FIELDS
    ziti_context ztx;
    ziti_connection conn;
    char *service;
} ziti_link_t;

// connect and release method for um_http custom source link
static int ziti_src_connect(tlsuv_src_t *src, const char *, const char *, tlsuv_src_connect_cb cb, void *conn_ctx);

static void ziti_src_release(tlsuv_src_t *src);

// uv_link methods
static int zl_read_start(uv_link_t *l);

static int zl_write(uv_link_t *link, uv_link_t *source, const uv_buf_t bufs[],
                    unsigned int nbufs, uv_stream_t *send_handle, uv_link_write_cb cb, void *arg);

void zl_close(uv_link_t *link, uv_link_t *source, uv_link_close_cb cb);

const char *zl_strerror(uv_link_t *link, int err);

static void zlnf_conn_cb(ziti_connection conn, int status);

static ssize_t zlnf_data_cb(ziti_connection conn, const uint8_t *data, ssize_t length);

static void zlnf_write_cb(ziti_connection conn, ssize_t status, void *ctx);

struct zl_write_req_s {
    ziti_link_t *zl;
    uv_link_write_cb cb;
    void *arg;
};

static const uv_link_methods_t ziti_link_methods = {
        .read_start = zl_read_start,
        .write = zl_write,
        .close = zl_close,
        .strerror = zl_strerror,
        .alloc_cb_override = NULL,
        .read_cb_override = NULL
};

int ziti_src_init(uv_loop_t *l, tlsuv_src_t *st, const char *svc, ziti_context ztx) {
    st->loop = l;
    st->connect = ziti_src_connect;
    st->connect_cb = NULL;
    st->release = ziti_src_release;
    st->link = malloc(sizeof(ziti_link_t));
    uv_link_init(st->link, &ziti_link_methods);

    ziti_link_t *zl = (ziti_link_t *) st->link;
    if (svc) {
        zl->service = strdup(svc);
    }
    else
        zl->service = NULL;
    zl->ztx = ztx;
    
    return 0; 
}

static int
ziti_src_connect(tlsuv_src_t *src, const char *host, const char *port, tlsuv_src_connect_cb cb, void *conn_ctx) {
    ziti_link_t *zl = (ziti_link_t *) src->link;

    ziti_address a;
    parse_ziti_address_str(&a, host);

    if (zl->service == NULL) { // find service by intercept
        int portnum = (int) strtol(port, NULL, 10);
        const ziti_service *s = ziti_service_for_addr(zl->ztx, ziti_protocols.tcp, &a, portnum);
        if (s == NULL) {
            ZITI_LOG(ERROR, "no service for address[tcp:%s:%s]", host, port);
            return ZITI_SERVICE_UNAVAILABLE;
        }

        zl->service = strdup(s->name);
    }

    ZITI_LOG(TRACE, "service %s", zl->service);
    src->connect_cb = cb;
    src->connect_ctx = conn_ctx;

    int status = ziti_conn_init(zl->ztx, &zl->conn, src);
    if (status != ZITI_OK) {
        return status;
    }

    char app_data[1024];
    size_t app_data_len = snprintf(app_data, sizeof(app_data),
                                   "{"
                                   "\"dst_protocol\":\"tcp\","
                                   "\"%s\":\"%s\", "
                                   "\"dst_port\":\"%s\""
                                   "}",
                                   a.type == ziti_address_cidr ? "dst_ip" : "dst_hostname",
                                   host, port);

    ziti_dial_opts opts = {
            .app_data = app_data,
            .app_data_sz = app_data_len,
    };


    return ziti_dial_with_options(zl->conn, zl->service, &opts, zlnf_conn_cb, zlnf_data_cb);
}

static void ziti_src_release(tlsuv_src_t *src) {
    ziti_link_t *zl = (ziti_link_t *) src->link;
    free(zl->service);
    free(src->link);
}

static void zlnf_conn_cb(ziti_connection conn, int status) {
    tlsuv_src_t *src = (tlsuv_src_t *) ziti_conn_data(conn);
    src->connect_cb(src, status, src->connect_ctx);
}

//static void link_close_cb(uv_link_t *l) {}
static ssize_t zlnf_data_cb(ziti_connection conn, const uint8_t *data, ssize_t length) {
    tlsuv_src_t *src = (tlsuv_src_t *) ziti_conn_data(conn);
    uv_buf_t read_buf;

    if (length == ZITI_EOF) {
        ZITI_LOG(TRACE, "ZITI_EOF");
        uv_link_propagate_read_cb(src->link, UV_EOF, NULL);
    }
    else if (length < 0) {
        ZITI_LOG(ERROR, "unexpected error: %s", ziti_errorstr(length));
        uv_link_propagate_read_cb(src->link, length, NULL);
    }
    else {
        ZITI_LOG(VERBOSE, "propagating read %zd bytes", length);
        uv_link_propagate_alloc_cb(src->link, 64 * 1024, &read_buf);
        if (read_buf.len == 0 || read_buf.base == NULL) {
            // client cannot accept any data ATM (UV_ENOBUFS)
            return 0;
        }

        if (length > read_buf.len) {
            length = (ssize_t) read_buf.len;
        }
        memcpy(read_buf.base, data, length);
        uv_link_propagate_read_cb(src->link, length, &read_buf);
    }

    return length;
}

static void zlnf_write_cb(ziti_connection conn, ssize_t status, void *ctx) {
    struct zl_write_req_s *req = ctx;
    req->cb((uv_link_t *) req->zl, (int) status, req->arg);
    free(req);
}

static int zl_read_start(uv_link_t *l) {
    return 0; 
}

static int zl_write(uv_link_t *link, uv_link_t *source, 
    const uv_buf_t bufs[], unsigned int nbufs, 
    uv_stream_t *send_handle, uv_link_write_cb cb, void *arg) {

    ziti_link_t *zl = (ziti_link_t *)link;
    struct zl_write_req_s *req = malloc(sizeof(struct zl_write_req_s));
    req->zl = zl;
    req->cb = cb;
    req->arg = arg;

    ZITI_LOG(TRACE, "%s, nbuf=%u, buf[0].len=%zu", zl->service, nbufs, bufs[0].len);
    return ziti_write(zl->conn, (uint8_t *) bufs[0].base, bufs[0].len, zlnf_write_cb, req);
}

void zl_close(uv_link_t* link, uv_link_t* source, uv_link_close_cb link_close_cb) {
    ziti_link_t *zl = (ziti_link_t *)link;

    ZITI_LOG(TRACE, "%s", zl->service);
    ziti_close(zl->conn, NULL);
    link_close_cb((uv_link_t *) zl);
}

const char* zl_strerror(uv_link_t* link, int err) {
    return ziti_errorstr(err);
}
