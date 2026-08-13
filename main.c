#include "http.h"

/* Echoes the request body, which is what the server did before routing
 * existed. */
static void
handle_echo (struct http_request const *request, struct http_reply *reply)
{
  reply->status = HTTP_CODE_200 H1_EOL;
  reply->content_type = "application/octet-stream";
  reply->content = (byte *)request->body;
  reply->length = request->body_length;
}

static void
handle_root (struct http_request const *request, struct http_reply *reply)
{
  (void)request;
  static char const greeting[] = "libedgerunner\n";
  reply->status = HTTP_CODE_200 H1_EOL;
  reply->content_type = "text/plain; charset=utf-8";
  reply->content = (byte *)greeting;
  reply->length = sizeof (greeting) - 1;
}

/* Demonstrates slug captures: GET /user/42 reports id=42. */
static void
handle_user (struct http_request const *request, struct http_reply *reply)
{
  usz len = 0;
  char const *id = http_capture (request, "id", &len);
  char *body = null;
  int written = asprintf (&body, "user id: %.*s\n", (int)len, id ? id : "");
  if (written < 0)
    {
      reply->status = HTTP_CODE_500 H1_EOL;
      return;
    }
  reply->status = HTTP_CODE_200 H1_EOL;
  reply->content_type = "text/plain; charset=utf-8";
  reply->content = (byte *)body;
  reply->length = written;
  reply->content_free = free;
}

int
main (int argc, char *argv[])
{
  if (argc < 3 || argc > 4)
    goto usage;
  auto port = atol (argv[2]);
  if (port < 1 || port > 65535)
    {
      cerr ("invalid port:", argv[2]);
      return 1;
    }
  struct sockaddr_storage addr;
  if (uv_ip4_addr (argv[1], port, (struct sockaddr_in *)&addr)
      && uv_ip6_addr (argv[1], port, (struct sockaddr_in6 *)&addr))
    {
      cerr ("invalid host:", argv[1]);
      return 1;
    }
  if (http_route (ROUTE_GET | ROUTE_HEAD, "/", handle_root)
      || http_route (ROUTE_GET | ROUTE_HEAD, "/user/{id}", handle_user)
      || http_route (ROUTE_POST | ROUTE_PUT, "/echo", handle_echo))
    {
      cerr ("failed to register routes");
      return 1;
    }
  long threads = argc == 4 ? atol (argv[3]) : 0;
  return http_listen ((struct sockaddr *)&addr, threads);
usage:
  cerr ("usage:", argv[0], "<host> <port> [threads]");
  return 1;
}
