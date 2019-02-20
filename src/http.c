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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "zmalloc.h"
#include "anet.h"
#include "http.h"

#define UNUSED(V) ((void) V)
#define REDIS_HTTP_DEFAULT_TCP_BACKLOG  511
#define NET_IP_STR_LEN                  46
#define IOBUF_LEN                       (1024*16)

typedef struct {
    char *filename;
    int fd;
    off_t size;
} redisHttpStaticFile;

typedef struct {
    char *ext;
    char *type;
} mimetype;

static mimetype mime_types[] = {
    {".css", "text/css"},
    {".gif", "image/gif"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".xml", "text/xml"},
    {NULL, NULL}
};

static uint64_t dictSdsHash(const void *key);
static int dictSdsKeyCompare(void *privdata, const void *key1,
    const void *key2);
static void dictSdsDestructor(void *privdata, void *val);
static dictType redisHttpDicType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictSdsDestructor,         /* key destructor */
    dictSdsDestructor          /* val destructor */
};

static uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

static int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    sdsfree(val);
}

sds redisHttpGetDictValue(dict *dict, sds key) {
    dictEntry *entry = dictFind(dict, key);
    if (entry == NULL) return NULL;
    return dictGetVal(entry);
}

void redisHttpLog(int level, redisHttpClient *c, const char* fmt, ...) {
    if (level > c->server->verbosity) return;
    FILE *out = (level == REDIS_HTTP_VERBOSITY_ERR ? stderr : stdout);
    fprintf(out, "%s - Redis HTTP: ", c->ip);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}

void redisHttpSetResponseHeader(redisHttpResponse *res, char *field,
                                char *value)
{
    dictReplace(res->header, sdsnew(field), sdsnew(value));
}

void redisHttpRedirect(redisHttpResponse *res, char *uri) {
    res->code = 303;
    res->status = "See Other";
    redisHttpSetResponseHeader(res, "Location", uri);
}

void redisHttpClientRelease(redisHttpClient *client) {
    redisHttpServer *server = client->server;
    if (client->fd > -1) {
        aeDeleteFileEvent(server->el, client->fd, AE_READABLE);
        aeDeleteFileEvent(server->el, client->fd, AE_WRITABLE);
        close(client->fd);
    }
    sdsfree(client->ip);
    if (client->buf) sdsfree(client->buf);
    zfree(client);
}

void redisHttpClientRemove(redisHttpClient *client) {
    redisHttpLogDebug(client, "Removing client\n");
    redisHttpServer *server = client->server;
    listIter li;
    listNode *ln, *ln2del = NULL;
    listRewind(server->clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        redisHttpClient *c = (redisHttpClient *) ln->value;
        if (c == client) {
            ln2del = ln;
            break;
        }
    }
    if (ln2del == NULL) return;
    listDelNode(server->clients, ln2del);
    redisHttpClientRelease(client);
}

static void parseNameValuePair(char *pair, sds *name, sds *value) {
    *name = NULL;
    *value = NULL;
    char *p = strchr(pair, '=');
    if (p != NULL) {
        *p = '\0';
        *name = sdsnew(pair);
        if (sdslen(*name) == 0) return;
        char *v = p+1;
        if (v[0] == '&') *value = sdsempty();
        else *value = sdsnew(v);
    }
}

static void parseQueryStringPair(char *pair, redisHttpRequest *r) {
    sds param = NULL, value = NULL;
    parseNameValuePair(pair, &param, &value);
    if (param != NULL && value != NULL) dictReplace(r->params, param, value);
}

static void parseCookiePair(char *pair, redisHttpRequest *r) {
    sds cookie = NULL, value = NULL;
    parseNameValuePair(pair, &cookie, &value);
    if (cookie != NULL && value != NULL) dictReplace(r->cookies, cookie, value);
}

static void processRequestLine(char *l, redisHttpRequest *r) {
    char *p = NULL;
    if (strcasestr(l, "GET ") == l || strcasestr(l, "POST ") == l ||
        strcasestr(l, "PUT ") == l || strcasestr(l, "PATCH ") == l ||
        strcasestr(l, "DELETE ") == l)
    {
        /* Method, path and query string. */
        p = strchr(l, ' ');
        *p = '\0';
        r->method = sdsnew(l);
        sdstoupper(r->method);
        l = p + 1;
        p = strstr(l, " HTTP");
        if (p != NULL) *p = '\0';
        char *path = l;
        p = strchr(path, '?');
        if (p != NULL) {
            *p = '\0';
            r->query = sdsnew(++p);
            char *q = p;
            while ((p = strchr(q, '&')) != NULL) {
                *p = '\0';
                char *parval = q;
                q = p + 1;
                parseQueryStringPair(parval, r);
            }
            if (strlen(q) > 0) parseQueryStringPair(q, r);
        }
        r->path = sdsnew(path);
    } else if ((p = strchr(l, ':')) != NULL) {
        /* Header */
        *p = '\0';
        sds name = sdsnew(l), value = NULL;
        if (sdslen(name) > 0) {
            value = sdsnew(p + 1);
            dictReplace(r->header, name, value);
        }
        /* Cookies */
        if (value && strcasecmp(name, "Cookie") == 0) {
            char *cookie = value;
            while ((p = strchr(cookie, ';')) != NULL) {
                *p = '\0';
                parseCookiePair(cookie, r);
                cookie = p + 1;
                /* Trim spaces */
                while (cookie[0] && cookie[0] == ' ') cookie++;
            }
            if (strlen(cookie) > 0) parseCookiePair(cookie, r);
        }
    }
}

static redisHttpRequest *redisHttpRequestCreate() {
    redisHttpRequest *r = zcalloc(sizeof(*r));
    r->params = dictCreate(&redisHttpDicType, NULL);
    r->header = dictCreate(&redisHttpDicType, NULL);
    r->cookies = dictCreate(&redisHttpDicType, NULL);
    return r;
}

static redisHttpResponse *redisHttpResponseCreate() {
    redisHttpResponse *r = zcalloc(sizeof(*r));
    r->header = dictCreate(&redisHttpDicType, NULL);
    r->cookies = dictCreate(&redisHttpDicType, NULL);
    r->body = sdsempty();
    redisHttpSetContentType(r, "text/html");
    return r;
}

static void redisHttpRequestRelease(redisHttpRequest *r) {
    if (r->method) sdsfree(r->method);
    if (r->path) sdsfree(r->path);
    if (r->query) sdsfree(r->query);
    if (r->argv) {
        int i;
        for (i = 0; i < r->argc; i++) sdsfree(r->argv[i]);
        zfree(r->argv);
    }
    dictRelease(r->params);
    dictRelease(r->header);
    dictRelease(r->cookies);
    zfree(r);
}

static void redisHttpResponseRelease(redisHttpResponse *r) {
    if (r->body) sdsfree(r->body);
    dictRelease(r->header);
    dictRelease(r->cookies);
    zfree(r);
}

static int matchRoute(redisHttpRequest *r, char *remaining, char *next,
                      sds route, sds pattern)
{
    int matched = 1, arg_idx = 0, argc = 0, i;
    sds *argv = NULL;
    char *route_next = NULL;
    char *cur_route_component = sdsnew("*");
    while (strlen(remaining) > 0) {
        int capture = (strcmp(cur_route_component, "*") == 0);
        int argv_len = 0;
        char *end_argv = strchr(remaining, '/');
        if (end_argv) argv_len = end_argv - remaining;
        else argv_len = strlen(remaining);
        sds arg = sdsnewlen(remaining, argv_len);
        if (capture) {
            arg_idx = argc++;
            argv = zrealloc(argv, argc * sizeof(*argv));
            argv[arg_idx] = arg;
        } else {
            matched = (strcmp(cur_route_component, arg) == 0);
            sdsfree(arg);
            if (!matched) goto cleanup;
        }
        remaining = next;
        if (remaining == NULL) {
            route_next = NULL;
            break;
        }
        next = strchr(remaining, '/');
        if (next) {
            next++;
            if (*next == 0) next = NULL;
        }
        route_next = route + sdslen(pattern);
        if (*route_next == '/') route_next++;
        char *next_route_sep = strchr(route_next, '/');
        int next_route_len =
            (next_route_sep ? next_route_sep - route_next : strlen(route_next));
        sdsfree(cur_route_component);
        cur_route_component = sdsnewlen(route_next, next_route_len);
        pattern = sdscat(pattern, cur_route_component);
    }
    if (route_next && strlen(route_next) > 0) matched = 0;
cleanup:
    if (matched) {
        for (i = 0; i < argc; i++) {
            arg_idx = r->argc++;
            r->argv = zrealloc(r->argv, r->argc * sizeof(*(r->argv)));
            r->argv[arg_idx] = argv[i];
        }
    } else for (i = 0; i < argc; i++) sdsfree(argv[i]);
    zfree(argv);
    return matched;
}

static redisHttpRouteHandler getRouteHandler(redisHttpClient *c,
                                             redisHttpRequest *r)
{
    sds routename = sdsnew(r->method);
    routename = sdscatfmt(routename, " %S", r->path);
    redisHttpRouteHandler handler = NULL;
    void *data = raxFind(c->server->routes, (unsigned char*) routename,
                         sdslen(routename));
    if (data != raxNotFound) {
        handler = (redisHttpRouteHandler) data;
        goto cleanup;
    }
    char *p = strstr(routename, r->path);
    assert(p != NULL);
    int idx = 0;
    if (p[0] == '/') p++;
    while ((p = strchr(p, '/')) != NULL) {
        /* First component cof the path must be a fixed string (ie. /user).
         * Don't proceed if there's nothing beyond the '/' (ie. /user/). */
        char *remaining = p + 1, *next = NULL;
        if (idx == 0 && *remaining == 0) break;
        int len = p - routename;
        sds pattern = sdsnewlen(routename, len);
        pattern = sdscat(pattern, "/*");
        /* If there's a further '/' after the component, add it to the
         * pattern. */
        if ((next = strchr(remaining, '/'))) {
            pattern = sdscat(pattern, "/");
            next++;
        }
        raxIterator iter;
        raxStart(&iter, c->server->routes);
        raxSeek(&iter,">=",(unsigned char*)pattern,sdslen(pattern));
        while (raxNext(&iter)) {
            redisHttpRouteHandler h = (redisHttpRouteHandler) iter.data;
            sds rte = sdsnewlen(iter.key, iter.key_len);
            if (strstr(rte, pattern) != rte) break;
            if (matchRoute(r, remaining, next, rte, pattern))
                handler = h;
            sdsfree(rte);
            if (handler) break;
        }
        raxStop(&iter);
        sdsfree(pattern);
        if (*(++p) == 0) break;
        idx++;
    }
cleanup:
    sdsfree(routename);
    return handler;
}

static void readStaticFile(redisHttpClient *c, redisHttpResponse *r,
                           redisHttpStaticFile *file)
{
    char *ext = strrchr(file->filename, '.'), *content_type = "text/plain";
    mimetype *mime = mime_types;
    while (mime->ext != NULL) {
        if (strcasecmp(mime->ext, ext) == 0) {
            content_type = mime->type;
            break;
        }
        mime++;
    }
    redisHttpSetContentType(r, content_type);
    if (sdslen(r->body) > 0) sdsclear(r->body);
    r->body = sdsMakeRoomFor(r->body, file->size);
    int nread, totread = 0, readlen = 1024;
    while ((nread = read(file->fd, r->body + sdslen(r->body), readlen)) > 0) {
        totread += nread;
    }
    close(file->fd);
    if (nread < 0) {
        redisHttpLogErr(c, "Failed to read static file: '%s'", file->filename);
        r->code = 500;
        r->status = "Internal Server Error";
        sdsclear(r->body);
        return;
    }
    sdsIncrLen(r->body, totread);
    /*r->body[sdslen(r->body)] = '\0';*/
}

static void buildResponseBuffer(redisHttpClient *c, redisHttpResponse *r,
                                redisHttpStaticFile *file)
{
    if (c->buf != NULL) sdsfree(c->buf);
    if (file != NULL) readStaticFile(c, r, file);
    c->buf = sdsnew("HTTP/1.1 ");
    c->buf = sdscatfmt(c->buf, " %U", r->code);
    if (r->status != NULL) c->buf = sdscatfmt(c->buf, " %s\r\n", r->status);
    else c->buf = sdscat(c->buf, "\r\n");
    dictIterator *iter = dictGetIterator(r->header);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        c->buf = sdscatfmt(c->buf, "%S: %S\r\n", dictGetKey(entry),
                           dictGetVal(entry));
    }
    dictReleaseIterator(iter);
    /* TODO: Cookies */
    c->buf = sdscatfmt(c->buf, "Content-length: %U\r\n\r\n", sdslen(r->body));
    c->buf = sdscat(c->buf, r->body);
    redisHttpLogDebug(c, "Response:\n%s\n", c->buf);
}

static void writeResponse(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    redisHttpClient *c = (redisHttpClient *) privdata;
    if (sdslen(c->buf) > c->written) {
        void *ptr = c->buf+c->written;
        ssize_t nwritten = write(c->fd,ptr,sdslen(c->buf) - c->written);
        if (nwritten == -1) {
            if (errno != EPIPE)
                redisHttpLogErr(c, "Writing to socket: %s\n", strerror(errno));
            redisHttpClientRemove(c);
            return;
        }
        c->written += nwritten;
    }
    if (sdslen(c->buf) == c->written) redisHttpClientRemove(c);
}

static void processRequest(redisHttpClient *c) {
    redisHttpLogDebug(c, "Request:\n%s\n", c->buf);
    redisHttpRequest *req = redisHttpRequestCreate();
    char *line = NULL, *p = c->buf;
    char *lines = p;
    while ((p = strchr(lines, '\n')) != NULL) {
        *p = '\0';
        line = lines;
        lines = p + 1;
        processRequestLine(line, req);
    }
    redisHttpResponse *res = redisHttpResponseCreate();
    if (req->method == NULL || req->path == NULL) {
        redisHttpLogErr(c, "Invalid request\n");
        res->code = 400;
        res->status = "Bad Request";
        goto write;
    }
    redisHttpRouteHandler handler = getRouteHandler(c, req);
    int found = (handler != NULL);
    redisHttpStaticFile static_file = {NULL, -1, 0};
    redisHttpStaticFile *file = NULL;
    if (!found && c->server->static_path != NULL) {
        /* TODO: url decode */
        sds fpath = sdsnew(c->server->static_path);
        if (fpath[sdslen(fpath) - 1] == '/' && *req->path == '/')
            fpath = sdscat(fpath, req->path + 1);
        else
            fpath = sdscat(fpath, req->path);
        redisHttpLogDebug(c, "Trying static path: %s\n", fpath);
        int ffd = open(fpath, O_RDONLY, 0);
        if (ffd > 0) {
            struct stat sbuf;
            fstat(ffd, &sbuf);
            /* TODO: implement automatic index.html for directories */
            /* TODO: implement symlinks */
            if (S_ISREG(sbuf.st_mode)) {
                found = 1;
                static_file.filename = strrchr(req->path, '/');
                if (static_file.filename == NULL)
                    static_file.filename = req->path;
                static_file.fd = ffd;
                static_file.size = sbuf.st_size;
                file = &static_file;
            }
        }
        sdsfree(fpath);
    }
write:
    if (aeCreateFileEvent(c->server->el, c->fd, AE_WRITABLE,
        writeResponse, c) == AE_ERR)
    {
        redisHttpLogErr(c, "Failed to create write event!\n");
        redisHttpClientRemove(c);
        goto cleanup;
    }
    if (!found && !res->code) {
        res->code = REDIS_HTTP_NOT_FOUND;
        res->status = REDIS_HTTP_MSG_NOT_FOUND;
    } else {
        if (handler) {
            int ok = handler(c, req, res);
            UNUSED(ok);
        }
        if (!res->code) res->code = REDIS_HTTP_OK;
        if (!res->status) res->status = REDIS_HTTP_MSG_OK;
    }
    buildResponseBuffer(c, res, file);
cleanup:
    if (req) redisHttpRequestRelease(req);
    if (res) redisHttpResponseRelease(res);
}

static void readRequest(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisHttpClient *c = (redisHttpClient *) privdata;
    int nread, readlen;
    size_t qblen;
    UNUSED(el);
    UNUSED(mask);

    readlen = IOBUF_LEN;
    qblen = sdslen(c->buf);
    c->buf = sdsMakeRoomFor(c->buf, readlen);
    nread = read(fd, c->buf+qblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            redisHttpLogErr(c, "Error reading from client: %s",
                            strerror(errno));
            redisHttpClientRemove(c);
            return;
        }
    } else if (nread == 0) {
        redisHttpLogInfo(c, "Client closed connection\n");
        redisHttpClientRemove(c);
        return;
    }
    if (sdslen(c->buf) > c->server->maxbodysize) {
        redisHttpLogInfo(c, "body size exceeded: %lu\n", sdslen(c->buf));
        redisHttpClientRemove(c);
        return;
    }
    processRequest(c);
}

static void acceptHttpHandler(aeEventLoop *el, int fd, void *privdata, int mask)
{
    int cport = 0, cfd = 0;
    UNUSED(mask);
    redisHttpServer *srv = (redisHttpServer *) privdata;

    char neterr[ANET_ERR_LEN] = {0};
    char cip[NET_IP_STR_LEN];
    if (listLength(srv->clients) >= srv->maxclients) {
        fprintf(stderr, "Redis HTTP Server: max clients reached, "
                        "rejecting connection\n");
        return;
    }
    cfd = anetTcpAccept(neterr, fd, cip, sizeof(cip), &cport);
    if (cfd == ANET_ERR) {
        fprintf(stderr, "Redis HTTP Server: error accepting client "
                        "connection: %s", neterr);
        return;
    }
    redisHttpClient *client = zcalloc(sizeof(*client));
    if (client == NULL) {
        fprintf(stderr, "Redis HTTP Server: failed to create client\n");
        return;
    }
    client->ip = sdsnew(cip);
    client->fd = cfd;
    client->buf = sdsempty();
    client->server = srv;
    redisHttpLogInfo(client, "Accept connection\n");
    if (aeCreateFileEvent(el, client->fd, AE_READABLE,
        readRequest, client) == AE_ERR)
    {
        redisHttpClientRelease(client);
        return;
    }
    listAddNodeTail(srv->clients, client);
}

void redisHttpAddRoute(redisHttpServer *s, const char * method, char *route,
                       redisHttpRouteHandler handler) {
    sds r = sdsnew(method);
    sdstoupper(r);
    r = sdscatfmt(r, " %s", route);
    if (s->verbosity >= REDIS_HTTP_VERBOSITY_DEBUG)
        printf("Adding route %s (%s, %s)\n", r, method, route);
    raxInsert(s->routes, (unsigned char *) r, sdslen(r),
              (void *) handler, NULL);
    sdsfree(r);
}

redisHttpServer *redisHttpServerCreate(int port) {
    if (!port) port = DEFAULT_REDIS_HTTP_PORT;
    redisHttpServer *srv = zcalloc(sizeof(*srv));
    if (srv == NULL) return NULL;
    srv->port = port;
    srv->routes = raxNew();
    srv->maxclients = DEFAULT_REDIS_HTTP_MAXCLIENTS;
    srv->maxbodysize = DEFAULT_REDIS_HTTP_MAXBODY_SIZE;
    srv->clients = listCreate();
    srv->verbosity = REDIS_HTTP_VERBOSITY_INFO;
    return srv;
}

int redisHttpServerStart(redisHttpServer *srv) {
    if (srv->el != NULL) {
        fprintf(stderr, "Redis HTTP Server Already started.\n");
        return 1;
    }
    srv->el = aeCreateEventLoop(srv->maxclients + 1);
    if (srv->el == NULL) {
        fprintf(stderr, "Failed to create event loop for HTTP Server\n");
        return 0;
    }
    char neterr[ANET_ERR_LEN] = {0};
    srv->conn_fd =
        anetTcp6Server(neterr, srv->port, NULL, REDIS_HTTP_DEFAULT_TCP_BACKLOG);
    if (srv->conn_fd != ANET_ERR) {
        anetNonBlock(NULL, srv->conn_fd);
    } else if (errno == EAFNOSUPPORT) {
        fprintf(stderr, "Redis HTTP Server: could not listen on %d\n%s\n",
                        srv->port, neterr);
        return 0;
    }
    if (aeCreateFileEvent(srv->el, srv->conn_fd, AE_READABLE,
                          acceptHttpHandler, srv) == AE_ERR) {
        fprintf(stderr, "Redis HTTP Server: failed to create main fd\n");
        return 0;
    }
    printf("Redis HTTP Server: starting on port %d\n", srv->port);
    aeMain(srv->el);
    return 1;
}

 void redisHttpServerRelease(redisHttpServer *httpserver) {
    if (httpserver->routes != NULL) raxFree(httpserver->routes);
    if (httpserver->clients != NULL) {
        listIter li;
        listNode *ln;
        listRewind(httpserver->clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            redisHttpClient *client = ln->value;
            redisHttpClientRelease(client);
        }
        listRelease(httpserver->clients);
    }
    if (httpserver->el != NULL) {
        aeStop(httpserver->el);
        aeDeleteEventLoop(httpserver->el);
    }
    zfree(httpserver);
 }
