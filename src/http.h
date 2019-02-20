/* A basic, lightweight HTTP Server implementation.
 *
 * Copyright (c) 2017-2018, Fabio Nicotra <artix2 at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_HTTP__
#define __REDIS_HTTP__

#include "sds.h"
#include "rax.h"
#include "dict.h"
#include "ae.h"
#include "adlist.h"

#define DEFAULT_REDIS_HTTP_PORT            9999
#define DEFAULT_REDIS_HTTP_MAXCLIENTS      10000000
#define DEFAULT_REDIS_HTTP_MAXBODY_SIZE    10485760

#define REDIS_HTTP_OK           200
#define REDIS_HTTP_NOT_FOUND    404

#define REDIS_HTTP_MSG_OK           "OK"
#define REDIS_HTTP_MSG_NOT_FOUND    "Not Found"

#define REDIS_HTTP_VERBOSITY_NONE   0
#define REDIS_HTTP_VERBOSITY_ERR    1
#define REDIS_HTTP_VERBOSITY_INFO   2
#define REDIS_HTTP_VERBOSITY_DEBUG  3

#define redisHttpLogInfo(...) \
    redisHttpLog(REDIS_HTTP_VERBOSITY_INFO, __VA_ARGS__)
#define redisHttpLogErr(...) \
    redisHttpLog(REDIS_HTTP_VERBOSITY_ERR, __VA_ARGS__)
#define redisHttpLogDebug(...) \
    redisHttpLog(REDIS_HTTP_VERBOSITY_DEBUG, __VA_ARGS__)

#define redisHttpGetHeader(obj, field) \
    (redisHttpGetDictValue(obj->header, field))
#define redisHttpGetArgument(req, idx) \
    (idx < req->argc ? req->argv[idx] : NULL)
#define redisHttpGetParam(req, param) \
    (redisHttpGetDictValue(req->params, param))

#define redisHttpSetContentType(res,type) \
    redisHttpSetResponseHeader(res, "Content-type", type)
#define redisHttpGet(s,r,h) redisHttpAddRoute(s,"GET",r,h)
#define redisHttpPost(s,r,h) redisHttpAddRoute(s,"POST",r,h)

typedef struct _redisHttpRequest {
    sds method;
    sds path;
    sds query;
    int argc;
    sds *argv;
    dict *params;
    dict *header;
    dict *cookies;
} redisHttpRequest;

typedef struct _redisHttpResponse {
    int code;
    char *status;
    dict *header;
    dict *cookies;
    sds body;
} redisHttpResponse;

typedef struct _redisHttpServer {
    int port;
    char *static_path;
    rax *routes;
    unsigned long maxclients;
    unsigned long maxbodysize;
    list *clients;
    aeEventLoop *el;
    int conn_fd;
    int verbosity;
    void *data;
 } redisHttpServer;

typedef struct _redisHttpClient {
    sds ip;
    int fd;
    sds buf;
    size_t written;
    redisHttpServer *server;
 } redisHttpClient;

typedef int (*redisHttpRouteHandler)(redisHttpClient *c,
                                     redisHttpRequest *req,
                                     redisHttpResponse *res);

void redisHttpLog(int level, redisHttpClient *c, const char* fmt, ...);
redisHttpServer *redisHttpServerCreate(int port);
int redisHttpServerStart(redisHttpServer *srv);
void redisHttpServerRelease(redisHttpServer *httpserver);
sds redisHttpGetDictValue(dict *dict, sds key);
void redisHttpSetResponseHeader(redisHttpResponse *res, char *field,
                                char *value);
void redisHttpAddRoute(redisHttpServer *s, const char * method, char *route,
                       redisHttpRouteHandler handler);
 #endif
