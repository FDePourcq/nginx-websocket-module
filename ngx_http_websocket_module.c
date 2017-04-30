#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_crypt.h>
#include <ngx_sha1.h>
#include <wslay/wslay.h>
#include "uthash.h"


struct ngx_http_ws_loc_conf_s {
    ngx_int_t pingintvl;
    ngx_int_t idleintvl;
};

typedef struct ngx_http_ws_loc_conf_s ngx_http_ws_loc_conf_t;

struct ngx_http_ws_ctx_s {
    ngx_http_request_t *r;
    wslay_event_context_ptr ws;
    ngx_event_t *ping_ev;
    ngx_event_t *timeout_ev;
    ngx_int_t pingintvl;
    ngx_int_t idleintvl;
    UT_hash_handle hh;
};

typedef struct ngx_http_ws_ctx_s ngx_http_ws_ctx_t;

ngx_http_ws_ctx_t *ws_ctx_hash = NULL;

struct ngx_http_ws_srv_addr_s {
    void *cscf; /** ngx_http_core_srv_conf_t **/
    ngx_str_t addr_text;
    UT_hash_handle hh;
};

typedef struct ngx_http_ws_srv_addr_s ngx_http_ws_srv_addr_t;

static ngx_http_ws_srv_addr_t *ws_srv_addr_hash = NULL;

static char *ngx_http_ws_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_ws_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_ws_process_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_ws_handshake(ngx_http_request_t *r);
static ngx_int_t ngx_http_ws_push(ngx_http_request_t *r);
static ngx_int_t ngx_http_ws_send_handshake(ngx_http_request_t *r);
static void ngx_http_ws_close(ngx_http_ws_ctx_t *t);
static void ngx_http_ws_add_timer(ngx_http_ws_ctx_t *t);
static void ngx_http_ws_send_push_token(ngx_http_ws_ctx_t *t);
static ngx_http_ws_ctx_t *ngx_http_ws_init_ctx(ngx_http_request_t *r);

static ngx_command_t ngx_http_ws_commands[] = {

    { ngx_string("websocket"),
      NGX_HTTP_LOC_CONF|NGX_CONF_ANY,
      ngx_http_ws_conf,
      0, /* No offset. Only one context is supported. */
      0, /* No offset when storing the module configuration on struct. */
      NULL },

    ngx_null_command
};

void *
ngx_http_ws_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_ws_loc_conf_t *lcf = ngx_palloc(cf->pool, sizeof(ngx_http_ws_loc_conf_t));
    lcf->pingintvl = 300 * 1000; // 5 min
    lcf->idleintvl = 360 * 1000; // 6 min

    return lcf;
}

static ngx_http_module_t ngx_http_websocket_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_ws_create_loc_conf, /* create location configuration */
    NULL  /* merge location configuration */
};

/* Module definition. */
ngx_module_t ngx_http_websocket_module = {
    NGX_MODULE_V1,
    &ngx_http_websocket_module_ctx, /* module context */
    ngx_http_ws_commands,           /* module directives */
    NGX_HTTP_MODULE,                /* module type */
    NULL,                           /* init master */
    NULL,                           /* init module */
    ngx_http_ws_process_init,       /* init process */
    NULL,                           /* init thread */
    NULL,                           /* exit thread */
    NULL,                           /* exit process */
    NULL,                           /* exit master */
    NGX_MODULE_V1_PADDING
};

#define add_header(header_key, header_value)                                 \
    h = ngx_list_push(&r->headers_out.headers);                              \
    if (h == NULL) {                                                         \
        return NGX_ERROR;                                                    \
    }                                                                        \
    h->hash = 1;                                                             \
    h->key.len = sizeof(header_key) - 1;                                     \
    h->key.data = (u_char *) header_key;                                     \
    h->value.len = strlen((const char *)header_value);                       \
    h->value.data = (u_char *) header_value

#define WS_UUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

ngx_table_elt_t *
ngx_http_ws_find_key_header(ngx_http_request_t *r)
{
    ngx_table_elt_t *key_header = NULL;
    ngx_list_part_t *part = &r->headers_in.headers.part;
    ngx_table_elt_t *headers = part->elts;
    for (ngx_uint_t i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            headers = part->elts;
            i = 0;
        }

        if (headers[i].hash == 0) {
            continue;
        }

        if (0 == ngx_strncmp(headers[i].key.data, (u_char *) "Sec-WebSocket-Key", headers[i].key.len)) {
            key_header = &headers[i];
        }
    }

    return key_header;
}

u_char *
ngx_http_ws_build_accept_key(ngx_table_elt_t *key_header, ngx_http_request_t *r)
{
    ngx_str_t encoded, decoded;
    ngx_sha1_t  sha1;
    u_char digest[20];

    decoded.len = sizeof(digest);
    decoded.data = digest;

    ngx_sha1_init(&sha1);
    ngx_sha1_update(&sha1, key_header->value.data, key_header->value.len);
    ngx_sha1_update(&sha1, WS_UUID, sizeof(WS_UUID) - 1);
    ngx_sha1_final(digest, &sha1);

    encoded.len = ngx_base64_encoded_length(decoded.len) + 1;
    encoded.data = ngx_pnalloc(r->pool, encoded.len);
    ngx_memzero(encoded.data, encoded.len);
    if (encoded.data == NULL) {
        return NULL;
    }

    ngx_encode_base64(&encoded, &decoded);

    return encoded.data;
}

static ssize_t
ngx_http_ws_recv_callback(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
        int flags, void *user_data)
{
    ngx_http_ws_ctx_t *t = user_data;
    ngx_http_request_t *r = t->r;
    ngx_connection_t  *c = r->connection;

    ssize_t n = recv(c->fd, buf, len, 0);
    if (n == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        } else {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        }
    } else if (n == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);

        n = -1;
    }

    return n;
}

static ssize_t
ngx_http_ws_send_callback(wslay_event_context_ptr ctx,
        const uint8_t *data, size_t len, int flags, void *user_data)
{
    ngx_http_ws_ctx_t *t = user_data;
    ngx_http_request_t *r = t->r;
    ngx_connection_t  *c = r->connection;

    ssize_t n = send(c->fd, data, len, 0);
    if (n == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        } else {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        }
    }

    return n;
}

void
ngx_http_ws_flush_timer(ngx_http_ws_ctx_t *t)
{
    ngx_add_timer(t->ping_ev, t->pingintvl);
    ngx_add_timer(t->timeout_ev, t->idleintvl);
}

void
ngx_http_ws_msg_callback(wslay_event_context_ptr ctx,
        const struct wslay_event_on_msg_recv_arg *arg, void *user_data)
{
    ngx_http_ws_ctx_t *t = user_data;

    ngx_http_ws_flush_timer(t);

    if(!wslay_is_ctrl_frame(arg->opcode)) {
        struct wslay_event_msg msg = {
            arg->opcode, arg->msg, arg->msg_length
        };
        wslay_event_queue_msg(ctx, &msg);
    } else if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
        ngx_http_ws_close(t);
    }
}

static void
ngx_http_ws_close(ngx_http_ws_ctx_t *t)
{
    ngx_http_request_t *r = t->r;
    HASH_FIND_PTR(ws_ctx_hash, &r, t);

    if (t) {
        printf("%p\n", t);
        HASH_DEL(ws_ctx_hash, t);
    }

    ngx_del_timer(t->ping_ev);
    ngx_del_timer(t->timeout_ev);
    ngx_pfree(r->pool, t->ping_ev);
    ngx_pfree(r->pool, t->timeout_ev);
    ngx_pfree(r->pool, t);

    r->count = 1;
    ngx_http_finalize_request(r, NGX_DONE);
}

static ngx_int_t
ngx_http_ws_process_init(ngx_cycle_t *cycle)
{
    int status;
    struct addrinfo hints = {};
    struct addrinfo *res, *p;
    char ipstr[33];
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char hostname[cycle->hostname.len + 1];
    hostname[cycle->hostname.len] = '\0';
    ngx_memcpy(hostname, cycle->hostname.data, cycle->hostname.len);

    if ((status = getaddrinfo(hostname, NULL, &hints, &res)) != 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "get ip: %s", gai_strerror(status));
    }

    ngx_http_ws_srv_addr_t *s;
    for (s = ws_srv_addr_hash; s != NULL; s = s->hh.next) {
        printf(">>>:%p\n", s->cscf);
        p = res;

        struct sockaddr_in *ip = (struct sockaddr_in *)p->ai_addr;
        inet_ntop(p->ai_family, (void *)&ip->sin_addr, ipstr, sizeof(ipstr));

        int listen_fd = socket(PF_INET, SOCK_STREAM, 0);

        if (bind(listen_fd, (struct sockaddr *)ip, sizeof(struct sockaddr_in)) == -1) {
            return NGX_ABORT;
        }

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(struct sockaddr_in);
        if (getsockname(listen_fd, (struct sockaddr *) &addr, &addr_len) == -1) {
            return NGX_ABORT;
        }
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "get ip: %s:%d", ipstr, ntohs(addr.sin_port));

        // ngx_conf_t
        ngx_conf_t conf;
        ngx_memzero(&conf, sizeof(ngx_conf_t));

        conf.temp_pool = ngx_create_pool(NGX_CYCLE_POOL_SIZE, cycle->log);
        if (conf.temp_pool == NULL) {
            return NGX_ABORT;
        }

        conf.ctx = cycle->conf_ctx[ngx_http_module.index];
        conf.cycle = cycle;
        conf.pool = cycle->pool;
        conf.log = cycle->log;
        // lsopt
        ngx_http_listen_opt_t lsopt;
        ngx_memzero(&lsopt, sizeof(ngx_http_listen_opt_t));

        struct sockaddr_in *sin = &lsopt.sockaddr.sockaddr_in;
        *sin = addr;

        lsopt.socklen = sizeof(struct sockaddr_in);

        lsopt.backlog = NGX_LISTEN_BACKLOG;
        lsopt.rcvbuf = -1;
        lsopt.sndbuf = -1;
        lsopt.wildcard = 0;

        (void) ngx_sock_ntop(&lsopt.sockaddr.sockaddr, lsopt.socklen,
                lsopt.addr, NGX_SOCKADDR_STRLEN, 1);

        if (ngx_http_add_listen(&conf, s->cscf, &lsopt) != NGX_OK) {
            return NGX_ABORT;
        }

        ngx_http_core_main_conf_t *cmcf = ngx_http_conf_get_module_main_conf((&conf), ngx_http_core_module);
        ngx_http_conf_port_t *port = cmcf->ports->elts;
        port += cmcf->ports->nelts - 1;
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "port %d", port->port);

        ngx_http_init_listening(&conf, port);
        ngx_listening_t *ls = cycle->listening.elts;
        ls += cycle->listening.nelts - 1;
        ls->fd = listen_fd;

        ngx_connection_t *c = ngx_get_connection(ls->fd, cycle->log);
        if (c == NULL) {
            return NGX_ERROR;
        }

        c->type = ls->type;
        c->log = &ls->log;

        c->listening = ls;
        ls->connection = c;

        ngx_event_t *rev = c->read;

        rev->log = c->log;
        rev->accept = 1;
        rev->handler = ngx_event_accept;
        printf(">>>%.*s\n", (int)ls->addr_text.len, ls->addr_text.data);
        s->addr_text = ls->addr_text;

        if (listen(ls->fd, NGX_LISTEN_BACKLOG) != 0) {
            return NGX_ERROR;
        }

        if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

        ngx_destroy_pool(conf.temp_pool);
    }

    freeaddrinfo(res);

    return NGX_OK;
}

static struct wslay_event_callbacks callbacks = {
    ngx_http_ws_recv_callback,
    ngx_http_ws_send_callback,
    NULL,
    NULL,
    NULL,
    NULL,
    ngx_http_ws_msg_callback
};

static void
ngx_http_ws_event_handler(ngx_http_request_t *r)
{
    ngx_connection_t *c = r->connection;
    wslay_event_context_ptr ctx = (wslay_event_context_ptr) r->upstream;

    if (c->read->ready) {
        wslay_event_recv(ctx);
    }

    if (c->write->ready) {
        wslay_event_send(ctx);
    }
}

static ngx_int_t ngx_http_ws_handler(ngx_http_request_t *r)
{
    if (r->method & NGX_HTTP_GET) {
        return ngx_http_ws_handshake(r);
    } else if (r->method & NGX_HTTP_POST) {
        return ngx_http_ws_push(r);
    } else if (r->method & NGX_HTTP_OPTIONS) {
        // TODO add allow methods
        return NGX_HTTP_NO_CONTENT;
    } else {
        return NGX_HTTP_NOT_ALLOWED;
    }
}

static void
ngx_http_ws_push_body_handler(ngx_http_request_t *r)
{
    if (r->request_body == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    // TODO iterate bufs
    // TODO process temp_file
    ngx_buf_t *buf = r->request_body->bufs->buf;
    printf("###:%.*s\n", (int)(buf->last - buf->pos), buf->pos);

    ngx_str_t user = r->headers_in.user;
    ngx_http_request_t *wsr = (ngx_http_request_t *) ngx_hextoi((u_char *)(user.data + 2), (size_t)user.len - 2);
    ngx_http_ws_ctx_t *t;
    HASH_FIND_PTR(ws_ctx_hash, &wsr, t);
    if (t == NULL) {
        return;
    }

    struct wslay_event_msg wsmsg = {
        // TODO process binary data
        WSLAY_TEXT_FRAME, buf->pos, buf->last - buf->pos
    };
    wslay_event_queue_msg(t->ws, &wsmsg);
    wslay_event_send(t->ws);

    ngx_http_finalize_request(r, NGX_HTTP_NO_CONTENT);
}

static ngx_int_t
ngx_http_ws_push(ngx_http_request_t *r)
{
    ngx_int_t rc = ngx_http_auth_basic_user(r);
    if (rc == NGX_ERROR) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_str_t user = r->headers_in.user;
    if (user.len == 0) {
        return NGX_HTTP_BAD_REQUEST;
    }

    // skip the leading 0x
    ngx_http_request_t *wsr = (ngx_http_request_t *) ngx_hextoi((u_char *)(user.data + 2), (size_t)user.len - 2);

    ngx_http_ws_ctx_t *t;
    HASH_FIND_PTR(ws_ctx_hash, &wsr, t);
    if (t == NULL) {
        return NGX_HTTP_BAD_REQUEST;
    }

    rc = ngx_http_read_client_request_body(r, ngx_http_ws_push_body_handler);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}

void
ngx_http_ws_ping(ngx_event_t *ev)
{
    printf("ping\n");
    ngx_http_ws_ctx_t *t = ev->data;

    struct wslay_event_msg msg = { WSLAY_PING, NULL, 0 };

    wslay_event_queue_msg(t->ws, &msg);
    wslay_event_send(t->ws);
}

void
ngx_http_ws_timeout(ngx_event_t *ev)
{
    printf("close\n");
    ngx_http_ws_ctx_t *t = ev->data;

    struct wslay_event_msg msg = { WSLAY_CONNECTION_CLOSE, NULL, 0 };

    wslay_event_queue_msg(t->ws, &msg);
    wslay_event_send(t->ws);
}

static ngx_int_t
ngx_http_ws_handshake(ngx_http_request_t *r)
{
    r->count++; /* prevent nginx close connection after upgrade */
    r->keepalive = 0;

    ngx_http_ws_send_handshake(r); /* TODO check send error */

    ngx_http_ws_ctx_t *t = ngx_http_ws_init_ctx(r);

    ngx_http_ws_send_push_token(t);
    ngx_http_ws_add_timer(t);

    return NGX_OK;
}

static ngx_int_t
ngx_http_ws_send_handshake(ngx_http_request_t *r)
{
    ngx_table_elt_t* h;

    ngx_table_elt_t *key_header = ngx_http_ws_find_key_header(r);

    if (key_header != NULL) {
        u_char *accept = ngx_http_ws_build_accept_key(key_header, r);

        if (accept == NULL) {
            return NGX_ERROR;
        }

        add_header("Sec-WebSocket-Accept", accept);
    }

    r->headers_out.status = NGX_HTTP_SWITCHING_PROTOCOLS;
    r->headers_out.status_line.data = (u_char *) "101 Switching Protocols";
    r->headers_out.status_line.len = sizeof("101 Switching Protocols") - 1;

    add_header("Upgrade", "websocket");
    add_header("Sec-WebSocket-Version", "13");

    ngx_http_send_header(r);

    return ngx_http_send_special(r, NGX_HTTP_FLUSH);
}

static ngx_http_ws_ctx_t *
ngx_http_ws_init_ctx(ngx_http_request_t *r)
{
    wslay_event_context_ptr ctx = NULL;
    ngx_http_ws_ctx_t *t = ngx_pnalloc(r->pool, sizeof(ngx_http_ws_ctx_t));
    t->r = r;

    ngx_http_ws_loc_conf_t *wlcf = r->loc_conf[ngx_http_websocket_module.ctx_index];

    t->pingintvl = wlcf->pingintvl;
    t->idleintvl = wlcf->idleintvl;

    printf("intvl:%ld:%ld\n", t->pingintvl, t->idleintvl);

    wslay_event_context_server_init(&ctx, &callbacks, t);

    r->read_event_handler = ngx_http_ws_event_handler;
    r->upstream = (ngx_http_upstream_t *) ctx;

    t->ws = ctx;
    HASH_ADD_PTR(ws_ctx_hash, r, t);

    return t;
}

static void
ngx_http_ws_send_push_token(ngx_http_ws_ctx_t *t)
{
    ngx_http_request_t *r = t->r;
    wslay_event_context_ptr ctx = t->ws;

    ngx_http_core_srv_conf_t *cscf = r->srv_conf[ngx_http_core_module.ctx_index];
    ngx_http_core_loc_conf_t *clcf = r->loc_conf[ngx_http_core_module.ctx_index];

    ngx_http_ws_srv_addr_t *push_addr;
    HASH_FIND_PTR(ws_srv_addr_hash, &cscf, push_addr);

    char msg_buf[256];
    int msg_buf_len = sprintf(msg_buf, "http://%p@%.*s%.*s", r,
            (int)push_addr->addr_text.len, push_addr->addr_text.data,
            (int)clcf->name.len, clcf->name.data);
    struct wslay_event_msg msgarg = {
        WSLAY_TEXT_FRAME, (uint8_t *)msg_buf, msg_buf_len
    };

    wslay_event_queue_msg(ctx, &msgarg);
    wslay_event_send(ctx);
}

static void
ngx_http_ws_add_timer(ngx_http_ws_ctx_t *t)
{
    ngx_event_t *ping_ev = ngx_pnalloc(t->r->pool, sizeof(ngx_event_t) * 2);
    ngx_event_t *timeout_ev = ping_ev++;

    ping_ev->data = t;
    ping_ev->log = t->r->connection->log;
    ping_ev->handler = ngx_http_ws_ping;

    timeout_ev->data = t;
    timeout_ev->log = t->r->connection->log;
    timeout_ev->handler = ngx_http_ws_timeout;

    t->ping_ev = ping_ev;
    t->timeout_ev = timeout_ev;

    ngx_http_ws_flush_timer(t);
}

static char *
ngx_http_ws_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;
    ngx_http_core_srv_conf_t *cscf;
    ngx_http_ws_loc_conf_t *wlcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_ws_handler;

    cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);
    ngx_http_ws_srv_addr_t *srv_addr = ngx_pnalloc(cf->pool, sizeof(ngx_http_ws_srv_addr_t));
    srv_addr->cscf = cscf;
    HASH_ADD_PTR(ws_srv_addr_hash, cscf, srv_addr);

    wlcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_websocket_module);
    ngx_str_t *value = cf->args->elts;
    for (ngx_uint_t n = 1; n < cf->args->nelts; n++) {
        printf("conf:%.*s\n", (int)value[n].len, value[n].data);
        if (ngx_strncmp(value[n].data, "pingintvl=", 10) == 0) {
            wlcf->pingintvl = ngx_atoi(value[n].data + 10, value[n].len - 10);
        }

        if (ngx_strncmp(value[n].data, "idleintvl=", 10) == 0) {
            wlcf->idleintvl = ngx_atoi(value[n].data + 10, value[n].len - 10);
        }
    }

    return NGX_CONF_OK;
}
