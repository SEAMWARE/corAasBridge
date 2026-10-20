//
// FILE            aasHttp.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// HTTP/1.1 to an AAS server - see header.
//
#include <errno.h>                                    // errno, EAGAIN, EINTR
#include <netdb.h>                                    // getaddrinfo
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf, sscanf
#include <stdlib.h>                                   // malloc, realloc, free, strtol
#include <string.h>                                   // strlen, strncasecmp, memcpy, memmove
#include <strings.h>                                  // strncasecmp
#include <sys/socket.h>                               // socket, connect, setsockopt
#include <sys/time.h>                                 // struct timeval
#include <unistd.h>                                   // close

#include <openssl/err.h>                              // ERR_*
#include <openssl/ssl.h>                              // SSL_*
#include <openssl/x509.h>                             // X509_verify_cert_error_string

#include "aasHttp.h"                                  // Own interface



//
// BODY_MAX - the largest response body read: one submodel, read whole
//
#define BODY_MAX  (16 * 1024 * 1024)



// -----------------------------------------------------------------------------
//
// error - why this thread's last request had no answer
//
static __thread char error[512];



// -----------------------------------------------------------------------------
//
// Conn - one connection: a socket, and its TLS session when the server is https
//
typedef struct Conn
{
  int   fd;
  SSL*  ssl;
} Conn;



// -----------------------------------------------------------------------------
//
// Reader - what has been read of a response and not yet consumed
//
typedef struct Reader
{
  Conn*  connP;
  char   buf[16 * 1024];
  int    len;
  int    pos;
} Reader;



// -----------------------------------------------------------------------------
//
// aasTargetInit -
//
const char* aasTargetInit(AasTarget* tP, const char* url, const char* authorization, const char* caFile, bool insecure)
{
  memset(tP, 0, sizeof(AasTarget));

  const char* rest;

  if (strncmp(url, "https://", 8) == 0)
  {
    tP->tls  = true;
    tP->port = 443;
    rest     = &url[8];
  }
  else if (strncmp(url, "http://", 7) == 0)
  {
    tP->port = 80;
    rest     = &url[7];
  }
  else
    return "server.url is http[s]://<host>[:<port>][/<base path>]";

  int hostLen = 0;

  while ((rest[hostLen] != 0) && (rest[hostLen] != ':') && (rest[hostLen] != '/'))
    hostLen++;

  if ((hostLen == 0) || (hostLen >= (int) sizeof(tP->host)))
    return "server.url has no host";

  memcpy(tP->host, rest, hostLen);
  tP->host[hostLen] = 0;
  rest = &rest[hostLen];

  if (*rest == ':')
  {
    tP->port = atoi(&rest[1]);
    if ((tP->port <= 0) || (tP->port > 65535))
      return "server.url has a port that is no TCP port";

    while ((*rest != 0) && (*rest != '/'))
      rest++;
  }

  //
  // The base path - where the server's API is rooted ("/api/v3.0" for some), without a last '/'
  //
  size_t baseLen = strlen(rest);

  while ((baseLen > 0) && (rest[baseLen - 1] == '/'))
    baseLen--;

  if (baseLen >= sizeof(tP->basePath))
    return "server.url has a base path that is too long";

  memcpy(tP->basePath, rest, baseLen);
  tP->basePath[baseLen] = 0;

  if (authorization != NULL)
  {
    if ((strlen(authorization) >= sizeof(tP->authorization)) || (strpbrk(authorization, "\r\n") != NULL))
      return "the Authorization header (server.authorizationEnv) is too long, or more than one line";

    strcpy(tP->authorization, authorization);
  }

  if (tP->tls == false)
    return NULL;

  SSL_CTX* ctxP = SSL_CTX_new(TLS_client_method());

  if (ctxP == NULL)
    return "no TLS context";

  if (insecure == true)
    SSL_CTX_set_verify(ctxP, SSL_VERIFY_NONE, NULL);
  else
  {
    bool loaded = (caFile != NULL) ? (SSL_CTX_load_verify_locations(ctxP, caFile, NULL) == 1) : (SSL_CTX_set_default_verify_paths(ctxP) == 1);

    if (loaded == false)
    {
      SSL_CTX_free(ctxP);
      return (caFile != NULL) ? "server.caFile is no PEM file of CA certificates" : "the system's CA certificates could not be loaded - server.caFile names them";
    }

    SSL_CTX_set_verify(ctxP, SSL_VERIFY_PEER, NULL);
    tP->verifyHost = true;
  }

  tP->sslCtx = ctxP;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// aasTargetRelease -
//
void aasTargetRelease(AasTarget* tP)
{
  if (tP->sslCtx != NULL)
    SSL_CTX_free((SSL_CTX*) tP->sslCtx);
  tP->sslCtx = NULL;
}



// -----------------------------------------------------------------------------
//
// connClose -
//
static void connClose(Conn* cP)
{
  if (cP->ssl != NULL)
  {
    SSL_shutdown(cP->ssl);
    SSL_free(cP->ssl);
    cP->ssl = NULL;
  }

  if (cP->fd >= 0)
    close(cP->fd);
  cP->fd = -1;
}



// -----------------------------------------------------------------------------
//
// connOpen - connected, and the TLS handshake done; every read and write times out after timeoutMs
//
static bool connOpen(AasTarget* tP, Conn* cP, int timeoutMs)
{
  struct addrinfo  hints;
  struct addrinfo* resP = NULL;
  char             port[16];

  cP->fd  = -1;
  cP->ssl = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", tP->port);

  int gai = getaddrinfo(tP->host, port, &hints, &resP);

  if (gai != 0)
  {
    snprintf(error, sizeof(error), "%.200s: %s", tP->host, gai_strerror(gai));
    return false;
  }

  struct timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };

  for (struct addrinfo* aP = resP; (aP != NULL) && (cP->fd < 0); aP = aP->ai_next)
  {
    int fd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol);

    if (fd < 0)
      continue;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, aP->ai_addr, aP->ai_addrlen) == 0)
      cP->fd = fd;
    else
    {
      snprintf(error, sizeof(error), "connect: %s", strerror(errno));
      close(fd);
    }
  }

  freeaddrinfo(resP);

  if (cP->fd < 0)
    return false;

  if (tP->tls == false)
    return true;

  cP->ssl = SSL_new((SSL_CTX*) tP->sslCtx);

  if ((cP->ssl == NULL) || (SSL_set_fd(cP->ssl, cP->fd) != 1))
  {
    connClose(cP);
    return false;
  }

  SSL_set_tlsext_host_name(cP->ssl, tP->host);

  if ((tP->verifyHost == true) && (SSL_set1_host(cP->ssl, tP->host) != 1))   // the certificate must be issued to the host's name
  {
    connClose(cP);
    return false;
  }

  if (SSL_connect(cP->ssl) != 1)
  {
    long verify = SSL_get_verify_result(cP->ssl);

    if (verify != X509_V_OK)
      snprintf(error, sizeof(error), "the server's certificate: %s", X509_verify_cert_error_string(verify));
    else
      snprintf(error, sizeof(error), "TLS handshake: %s", ERR_reason_error_string(ERR_peek_last_error()) ? ERR_reason_error_string(ERR_peek_last_error()) : "failed");

    ERR_clear_error();
    connClose(cP);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// connRead - >0 bytes read, 0 closed, -1 failed, -2 timed out (nothing read)
//
static int connRead(Conn* cP, char* buf, int size)
{
  if (cP->ssl == NULL)
  {
    ssize_t n = recv(cP->fd, buf, size, 0);

    if (n >= 0)
      return (int) n;

    return ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) ? -2 : -1;
  }

  int n = SSL_read(cP->ssl, buf, size);

  if (n > 0)
    return n;

  int e = SSL_get_error(cP->ssl, n);

  if (e == SSL_ERROR_ZERO_RETURN)
    return 0;

  if ((e == SSL_ERROR_WANT_READ) || ((e == SSL_ERROR_SYSCALL) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))))
  {
    ERR_clear_error();
    return -2;
  }

  ERR_clear_error();
  return (n == 0) ? 0 : -1;
}



// -----------------------------------------------------------------------------
//
// connWrite - all of it; false: failed
//
static bool connWrite(Conn* cP, const char* buf, int len)
{
  while (len > 0)
  {
    int n = (cP->ssl != NULL) ? SSL_write(cP->ssl, buf, len) : (int) send(cP->fd, buf, len, MSG_NOSIGNAL);

    if (n <= 0)
    {
      ERR_clear_error();
      return false;
    }

    buf += n;
    len -= n;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// readerFill - more into the buffer; as connRead. stopP: a timeout is retried until it is set
//
static int readerFill(Reader* rP, volatile bool* stopP)
{
  if (rP->pos > 0)
  {
    memmove(rP->buf, &rP->buf[rP->pos], rP->len - rP->pos);
    rP->len -= rP->pos;
    rP->pos  = 0;
  }

  if (rP->len >= (int) sizeof(rP->buf))
    return -1;                                        // a line longer than the buffer

  for (;;)
  {
    int n = connRead(rP->connP, &rP->buf[rP->len], sizeof(rP->buf) - rP->len);

    if (n > 0)
      rP->len += n;

    if ((n == -2) && (stopP != NULL) && (*stopP == false))
      continue;

    return n;
  }
}



// -----------------------------------------------------------------------------
//
// readLine - one line, without its CRLF; its length, or -1 (closed, failed, too long)
//
static int readLine(Reader* rP, char* line, int size, volatile bool* stopP)
{
  for (;;)
  {
    for (int ix = rP->pos; ix < rP->len; ix++)
    {
      if (rP->buf[ix] != '\n')
        continue;

      int len = ix - rP->pos;

      if ((len > 0) && (rP->buf[ix - 1] == '\r'))
        len--;

      if (len >= size)
        return -1;

      memcpy(line, &rP->buf[rP->pos], len);
      line[len] = 0;
      rP->pos   = ix + 1;
      return len;
    }

    if (readerFill(rP, stopP) <= 0)
      return -1;
  }
}



// -----------------------------------------------------------------------------
//
// readBytes - up to n bytes of what is buffered, reading when nothing is; how many (0: closed, -1 failed)
//
static int readBytes(Reader* rP, char* out, int n, volatile bool* stopP)
{
  if (rP->pos == rP->len)
  {
    int r = readerFill(rP, stopP);

    if (r <= 0)
      return r;
  }

  int have = rP->len - rP->pos;
  int take = (have < n) ? have : n;

  memcpy(out, &rP->buf[rP->pos], take);
  rP->pos += take;
  return take;
}



// -----------------------------------------------------------------------------
//
// Body - a response's body, malloc'd
//
typedef struct Body
{
  char*  buf;
  int    len;
  bool   tooBig;
} Body;



// -----------------------------------------------------------------------------
//
// bodyAdd - bytes of the body
//
static void bodyAdd(Body* bP, const char* bytes, int n)
{
  if ((bP->tooBig == true) || (bP->len + n > BODY_MAX))
  {
    bP->tooBig = true;
    return;
  }

  char* b = realloc(bP->buf, bP->len + n + 1);

  if (b == NULL)
  {
    bP->tooBig = true;
    return;
  }

  memcpy(&b[bP->len], bytes, n);
  bP->buf          = b;
  bP->len         += n;
  bP->buf[bP->len] = 0;
}



// -----------------------------------------------------------------------------
//
// exchange - the request sent and its response read: the status, the body to bP
//
static int exchange(AasTarget* tP, const char* method, const char* path, const char* body, const char* accept, Body* bP, int timeoutMs, volatile bool* stopP)
{
  Conn conn;

  error[0] = 0;

  if (connOpen(tP, &conn, timeoutMs) == false)
    return 0;

  int   bodyLen = (body != NULL) ? (int) strlen(body) : 0;
  char  auth[sizeof(tP->authorization) + 32] = "";
  char  head[8192];

  if (tP->authorization[0] != 0)
    snprintf(auth, sizeof(auth), "Authorization: %s\r\n", tP->authorization);

  int   headLen = snprintf(head, sizeof(head),
                           "%s %s%s HTTP/1.1\r\nHost: %s\r\n%sAccept: %s\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n",
                           method, tP->basePath, path, tP->host, auth, accept, (body != NULL) ? "Content-Type: application/json\r\n" : "", bodyLen);

  if ((headLen >= (int) sizeof(head)) || (connWrite(&conn, head, headLen) == false) || ((bodyLen > 0) && (connWrite(&conn, body, bodyLen) == false)))
  {
    snprintf(error, sizeof(error), "the request could not be sent");
    connClose(&conn);
    return 0;
  }

  Reader* rP = malloc(sizeof(Reader));

  if (rP == NULL)
  {
    connClose(&conn);
    return 0;
  }

  rP->connP = &conn;
  rP->len   = 0;
  rP->pos   = 0;

  char line[4096];
  int  status        = 0;
  int  contentLength = -1;
  bool chunked       = false;

  if ((readLine(rP, line, sizeof(line), stopP) < 0) || (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1))
  {
    snprintf(error, sizeof(error), "no HTTP response");
    status = 0;
  }

  for (int len; (status != 0) && ((len = readLine(rP, line, sizeof(line), stopP)) > 0); )
  {
    if (strncasecmp(line, "Content-Length:", 15) == 0)
      contentLength = atoi(&line[15]);
    else if ((strncasecmp(line, "Transfer-Encoding:", 18) == 0) && (strstr(&line[18], "chunked") != NULL))
      chunked = true;
  }

  char chunk[4096];

  if ((status != 0) && (chunked == true))
  {
    for (;;)
    {
      if ((stopP != NULL) && (*stopP == true))
        break;

      if (readLine(rP, line, sizeof(line), stopP) < 0)
        break;

      long size = strtol(line, NULL, 16);

      if (size <= 0)
        break;

      while (size > 0)
      {
        int n = readBytes(rP, chunk, (size < (long) sizeof(chunk)) ? (int) size : (int) sizeof(chunk), stopP);

        if (n <= 0)
          break;

        bodyAdd(bP, chunk, n);
        size -= n;
      }

      if ((size > 0) || (readLine(rP, line, sizeof(line), stopP) < 0))
        break;
    }
  }
  else if (status != 0)
  {
    int left = contentLength;

    while ((left != 0) && ((stopP == NULL) || (*stopP == false)))
    {
      int n = readBytes(rP, chunk, ((left > 0) && (left < (int) sizeof(chunk))) ? left : (int) sizeof(chunk), stopP);

      if (n <= 0)
        break;

      bodyAdd(bP, chunk, n);
      if (left > 0)
        left -= n;
    }
  }

  free(rP);
  connClose(&conn);
  return status;
}



// -----------------------------------------------------------------------------
//
// aasRequest -
//
int aasRequest(AasTarget* tP, const char* method, const char* path, const char* body, char** bodyP, int timeoutMs)
{
  Body* bP = calloc(1, sizeof(Body));

  *bodyP = NULL;

  if (bP == NULL)
    return 0;

  int status = exchange(tP, method, path, body, "application/json", bP, timeoutMs, NULL);

  if ((bP->tooBig == false) && (bP->buf != NULL))
    *bodyP = bP->buf;
  else
    free(bP->buf);

  free(bP);
  return status;
}



// -----------------------------------------------------------------------------
//
// aasHttpError -
//
const char* aasHttpError(void)
{
  return (error[0] != 0) ? error : "no answer";
}



// -----------------------------------------------------------------------------
//
// aasHttpVersion -
//
const char* aasHttpVersion(void)
{
  return OpenSSL_version(OPENSSL_VERSION);
}
