#pragma once
#ifndef _H_HTTP_
#define _H_HTTP_ 1

#include "cmacs.h"
#include "router.h"
#include <grimoire.h>
#include <llhttp.h>
#include <uv.h>

#define HTTP_CODE_100 "100 Continue"
#define HTTP_CODE_101 "101 Switching Protocols"
#define HTTP_CODE_102 "102 Processing"
#define HTTP_CODE_103 "103 Early Hints"
#define HTTP_CODE_200 "200 OK"
#define HTTP_CODE_201 "201 Created"
#define HTTP_CODE_202 "202 Accepted"
#define HTTP_CODE_203 "203 Non-Authoritative Information"
#define HTTP_CODE_204 "204 No Content"
#define HTTP_CODE_205 "205 Reset Content"
#define HTTP_CODE_206 "206 Partial Content"
#define HTTP_CODE_207 "207 Multi-Status"
#define HTTP_CODE_208 "208 Already Reported"
#define HTTP_CODE_226 "226 IM Used"
#define HTTP_CODE_300 "300 Multiple Choices"
#define HTTP_CODE_301 "301 Moved Permanently"
#define HTTP_CODE_302 "302 Found"
#define HTTP_CODE_303 "303 See Other"
#define HTTP_CODE_304 "304 Not Modified"
#define HTTP_CODE_305 "305 Use Proxy"
#define HTTP_CODE_306 "306 Switch Proxy"
#define HTTP_CODE_307 "307 Temporary Redirect"
#define HTTP_CODE_308 "308 Permanent Redirect"
#define HTTP_CODE_400 "400 Bad Request"
#define HTTP_CODE_401 "401 Unauthorized"
#define HTTP_CODE_402 "402 Payment Required"
#define HTTP_CODE_403 "403 Forbidden"
#define HTTP_CODE_404 "404 Not Found"
#define HTTP_CODE_405 "405 Method Not Allowed"
#define HTTP_CODE_406 "406 Not Acceptable"
#define HTTP_CODE_407 "407 Proxy Authentication Required"
#define HTTP_CODE_408 "408 Request Timeout"
#define HTTP_CODE_409 "409 Conflict"
#define HTTP_CODE_410 "410 Gone"
#define HTTP_CODE_411 "411 Length Required"
#define HTTP_CODE_412 "412 Precondition Failed"
#define HTTP_CODE_413 "413 Content Too Large"
#define HTTP_CODE_414 "414 URI Too Long"
#define HTTP_CODE_415 "415 Unsupported Media Type"
#define HTTP_CODE_416 "416 Range Not Satisfiable"
#define HTTP_CODE_417 "417 Expectation Failed"
#define HTTP_CODE_418 "418 I'm a teapot"
#define HTTP_CODE_421 "421 Misdirected Request"
#define HTTP_CODE_422 "422 Unprocessable Content"
#define HTTP_CODE_423 "423 Locked"
#define HTTP_CODE_424 "424 Failed Dependency"
#define HTTP_CODE_425 "425 Too Early"
#define HTTP_CODE_426 "426 Upgrade Required"
#define HTTP_CODE_428 "428 Precondition Required"
#define HTTP_CODE_429 "429 Too Many Requests"
#define HTTP_CODE_431 "431 Request Header Fields Too Large"
#define HTTP_CODE_451 "451 Unavailable For Legal Reasons"
#define HTTP_CODE_500 "500 Internal Server Error"
#define HTTP_CODE_501 "501 Not Implemented"
#define HTTP_CODE_502 "502 Bad Gateway"
#define HTTP_CODE_503 "503 Service Unavailable"
#define HTTP_CODE_504 "504 Gateway Timeout"
#define HTTP_CODE_505 "505 HTTP Version Not Supported"
#define HTTP_CODE_506 "506 Variant Also Negotiates"
#define HTTP_CODE_507 "507 Insufficient Storage"
#define HTTP_CODE_508 "508 Loop Detected"
#define HTTP_CODE_510 "510 Not Extended"
#define HTTP_CODE_511 "511 Network Authentication Required"

#define H1 "HTTP/1.1"
#define H1_EOL "\r\n"
#define H1_SERVER "Server: %s\r\n"
#define H1_CONNECTION "Connection: %s\r\n"
#define H1_CONTENT_LENGTH "Content-Length: %zu\r\n"
#define H1_CONTENT_TYPE "Content-Type: %s\r\n"
#define H1_ALLOW "Allow: %s\r\n"

/* Longest possible Allow value: every method name plus ", " separators. */
#define H1_ALLOW_MAX sizeof ("GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS")

struct http_header
{
  char *value;
  struct binstore;
  char field[0];
};

/* Per-event-loop routing context, private to http.c. Each worker loop owns
 * one: libr3 match state is mutated during matching and cannot be shared. */
struct http_loop;

struct http_client
{
  uv_tcp_t tcp_handle;
  llhttp_t parser;
  struct lsnod response_queue;
  struct http_loop *loop;
  bsto *url;
  bsto *header;
  bsto *body;
  bool closing : 1;
};

/* Handler contract: return an HTTP_CODE_* status line for the response body it
 * writes into *content / *length. Ownership of *content stays with the
 * handler unless *content_free is set. */
struct http_request
{
  struct http_client *client;
  llhttp_method_t method;
  char const *path;
  usz path_length;
  struct router_capture const *capture;
  usz capture_count;
  byte const *body;
  usz body_length;
};

struct http_reply
{
  char const *status;
  char const *content_type;
  byte *content;
  usz length;
  void (*content_free) (void *);
};

typedef void (*http_handler) (struct http_request const *request,
                              struct http_reply *reply);

/* Registers a handler before http_listen; the route table is read-only once
 * listening starts. Returns 0 on success. */
int http_route (int methods, char const *path, http_handler handler);

/* Look up a capture by slug name, e.g. the "id" of "/user/{id}". Returns null
 * when absent. The value is not null-terminated; use *length. */
char const *http_capture (struct http_request const *request,
                          char const *name, usz *length);

struct http_response
{
  struct http_client *client;
  struct lsnod list_entry;
  uv_write_t write_request;
  uv_buf_t write_buffer;
  bool keep_alive : 1;
  char buffer[0];
};

int http_listen (struct sockaddr const *addr, long threads);

#endif /* _H_HTTP_ */
