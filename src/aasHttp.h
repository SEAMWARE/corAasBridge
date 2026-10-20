#ifndef AAS_HTTP_H_
#define AAS_HTTP_H_

//
// FILE            aasHttp.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP/1.1 to an AAS server (IDTA Part 2, the AAS HTTP/REST API) - plain or TLS (OpenSSL), one request
// per connection (Connection: close), a body by Content-Length, chunked or up to the close.
//
// TLS has a context of the plugin's own: the server's certificate is verified against the system's CA
// certificates, or those of caFile, and must be issued to the url's host - or, insecure, not verified.
// The broker's own TLS settings are not touched.
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// AasTarget - where the server is, and how it is reached
//
typedef struct AasTarget
{
  bool   tls;
  char   host[256];
  int    port;
  char   basePath[1024];                              // the API's root on the server, "" or "/api/v3.0" - no last '/'
  char   authorization[2048];                         // "": none; else the Authorization header's value
  bool   verifyHost;                                  // the certificate must be issued to host
  void*  sslCtx;                                      // SSL_CTX*, NULL for plain HTTP
} AasTarget;



// -----------------------------------------------------------------------------
//
// aasTargetInit - from "http[s]://<host>[:<port>][/<base path>]". authorization (NULL: none): the value of
// an Authorization header sent with every request ("Bearer ..."). caFile (NULL: the system's): the PEM of
// the CAs the server's certificate is verified against; insecure: not verified. NULL: ok, else why not
//
extern const char* aasTargetInit(AasTarget* tP, const char* url, const char* authorization, const char* caFile, bool insecure);



// -----------------------------------------------------------------------------
//
// aasTargetRelease -
//
extern void aasTargetRelease(AasTarget* tP);



// -----------------------------------------------------------------------------
//
// aasRequest - one request, path below the base path; the status (0: no answer), the body malloc'd in
// *bodyP (NULL: none)
//
extern int aasRequest(AasTarget* tP, const char* method, const char* path, const char* body, char** bodyP, int timeoutMs);



// -----------------------------------------------------------------------------
//
// aasHttpError - why the calling thread's last request had no answer (status 0)
//
extern const char* aasHttpError(void);



// -----------------------------------------------------------------------------
//
// aasHttpVersion - the TLS library linked, for GET /version
//
extern const char* aasHttpVersion(void);

#endif  // AAS_HTTP_H_
