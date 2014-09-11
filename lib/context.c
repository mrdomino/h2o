/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>
#include "h2o.h"
#include "h2o/http1.h"
#include "h2o/http2.h"

void h2o_context_init(h2o_context_t *ctx, h2o_loop_t *loop)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->loop = loop;
    h2o_timeout_init(ctx->loop, &ctx->zero_timeout, 0);
    h2o_timeout_init(ctx->loop, &ctx->req_timeout, H2O_DEFAULT_REQ_TIMEOUT);
    h2o_prepend_chunked_filter(ctx);
    h2o_init_mimemap(&ctx->mimemap, H2O_DEFAULT_MIMETYPE);
    ctx->server_name = h2o_buf_init(H2O_STRLIT("h2o/0.1"));
    ctx->max_request_entity_size = H2O_DEFAULT_MAX_REQUEST_ENTITY_SIZE;
    ctx->http1_upgrade_to_http2 = H2O_DEFAULT_HTTP1_UPGRADE_TO_HTTP2;
    ctx->http2_max_concurrent_requests_per_connection = H2O_DEFAULT_HTTP2_MAX_CONCURRENT_REQUESTS_PER_CONNECTION;
}

void h2o_context_dispose(h2o_context_t *ctx)
{
#define DISPOSE_LINKED(type, entries) do { \
    while (entries != NULL) { \
        type *e = entries; \
        if (e->dispose != NULL) \
            e->dispose(e); \
        entries = e->next; \
        free(e); \
    } \
} while (0)

    DISPOSE_LINKED(h2o_handler_t, ctx->handlers);
    DISPOSE_LINKED(h2o_filter_t, ctx->filters);
    DISPOSE_LINKED(h2o_logger_t, ctx->loggers);
    h2o_dispose_mimemap(&ctx->mimemap);

#undef DISPOSE_LINKED
}

h2o_handler_t *h2o_prepend_handler(h2o_context_t *context, size_t sz, int (*on_req)(h2o_handler_t *self, h2o_req_t *req))
{
    h2o_handler_t *handler = h2o_malloc(sz);

    memset(handler, 0, sz);
    handler->next = context->handlers;
    handler->on_req = on_req;

    context->handlers = handler;

    return handler;
}

h2o_filter_t *h2o_prepend_filter(h2o_context_t *context, size_t sz, void (*on_start_response)(h2o_filter_t *self, h2o_req_t *req))
{
    h2o_filter_t *filter = h2o_malloc(sz);

    memset(filter, 0, sz);
    filter->next = context->filters;
    filter->on_start_response = on_start_response;

    context->filters = filter;

    return filter;
}

h2o_logger_t *h2o_prepend_logger(h2o_context_t *context, size_t sz, void (*log)(h2o_logger_t *self, h2o_req_t *req))
{
    h2o_logger_t *logger = h2o_malloc(sz);

    memset(logger, 0, sz);
    logger->next = context->loggers;
    logger->log = log;

    context->loggers = logger;

    return logger;
}

void h2o_get_timestamp(h2o_context_t *ctx, h2o_mempool_t *pool, h2o_timestamp_t *ts)
{
    uint64_t now = h2o_now(ctx->loop);

    if (ctx->_timestamp_cache.uv_now_at != now) {
        time_t prev_sec = ctx->_timestamp_cache.tv_at.tv_sec;
        ctx->_timestamp_cache.uv_now_at = now;
        gettimeofday(&ctx->_timestamp_cache.tv_at, NULL);
        if (ctx->_timestamp_cache.tv_at.tv_sec != prev_sec) {
            /* update the string cache */
            if (ctx->_timestamp_cache.value != NULL)
                h2o_mempool_release_shared(ctx->_timestamp_cache.value);
            ctx->_timestamp_cache.value = h2o_mempool_alloc_shared(NULL, sizeof(h2o_timestamp_string_t));
            h2o_time2str_rfc1123(ctx->_timestamp_cache.value->rfc1123, ctx->_timestamp_cache.tv_at.tv_sec);
            h2o_time2str_log(ctx->_timestamp_cache.value->log, ctx->_timestamp_cache.tv_at.tv_sec);
        }
    }

    ts->at = ctx->_timestamp_cache.tv_at;
    h2o_mempool_link_shared(pool, ctx->_timestamp_cache.value);
    ts->str = ctx->_timestamp_cache.value;
}

static void on_ssl_handshake_complete(h2o_socket_t *sock, int status)
{
    const h2o_buf_t *ident;
    h2o_context_t *ctx = sock->data;
    sock->data = NULL;

    h2o_buf_t proto;
    if (status != 0) {
        h2o_socket_close(sock);
        return;
    }

    proto = h2o_socket_ssl_get_selected_protocol(sock);
    for (ident = h2o_http2_tls_identifiers; ident->len != 0; ++ident) {
        if (proto.len == ident->len && memcmp(proto.base, ident->base, proto.len) == 0) {
            goto Is_Http2;
        }
    }
    /* connect as http1 */
    h2o_http1_accept(ctx, sock);
    return;

Is_Http2:
    /* connect as http2 */
    h2o_http2_accept(ctx, sock);
}

void h2o_accept(h2o_context_t *ctx, h2o_socket_t *sock)
{
    if (ctx->ssl_ctx != NULL) {
        sock->data = ctx;
        h2o_socket_ssl_server_handshake(sock, ctx->ssl_ctx, on_ssl_handshake_complete);
    } else {
        h2o_http1_accept(ctx, sock);
    }
}