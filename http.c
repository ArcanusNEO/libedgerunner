#include "http.h"
static llhttp_settings_t llhttp_settings;
static struct router *http_router;

/* One per event loop. The tree is private because r3_tree_matchl writes into
 * per-node PCRE2 match data; match is reused so lookups do not allocate. */
struct http_loop
{
  struct router_tree *tree;
  struct router_match match;
};

static char *EMPTYCSTR = "";
static char *H1_RESPONSE_400 = "HTTP/1.1 " HTTP_CODE_400 "\r\n"
                               "Connection: close\r\n"
                               "Content-Length: 11\r\n"
                               "\r\n"
                               "Bad Request";
static char *H1_RESPONSE_404 = "HTTP/1.1 " HTTP_CODE_404 "\r\n"
                               "Connection: close\r\n"
                               "Content-Length: 9\r\n"
                               "\r\n"
                               "Not Found";

static void
free_header (struct http_client *client)
{
  if (!client || !client->header)
    return;
  auto header = (struct http_header **)client->header->store;
  usz nr = client->header->size / sizeof (header[0]);
  for (usz i = 0; i < nr; ++i)
    free (header[i]);
  free (client->header);
  client->header = null;
}

static void
free_request (struct http_client *client)
{
  if (!client)
    return;
  free (client->body);
  client->body = null;
  free_header (client);
  free (client->url);
  client->url = null;
}

static void
free_client (struct http_client *client)
{
  if (!client)
    return;
  while (client->response_queue.next != &client->response_queue)
    {
      struct http_response *response = container_of (
          client->response_queue.next, struct http_response, list_entry);
      list$ (rem) (&response->list_entry);
      free (response);
    }
  free_request (client);
  free (client);
}

static void
close_client (struct http_client *client)
{
  if (client->closing)
    return;
  client->closing = true;
  uv_close ((uv_handle_t *)client, (uv_close_cb)free_client);
}

static void write_next (struct http_client *client);

static void
on_write (uv_write_t *request, int status)
{
  struct http_response *response
      = container_of (request, struct http_response, write_request);
  struct http_client *client = response->client;
  bool keep_alive = response->keep_alive;
  list$ (rem) (&response->list_entry);
  free (response);
  if (status || !keep_alive)
    close_client (client);
  else
    write_next (client);
}

static void
write_next (struct http_client *client)
{
  if (client->response_queue.next == &client->response_queue)
    return;
  struct http_response *response = container_of (
      client->response_queue.next, struct http_response, list_entry);
  int wr = uv_write (&response->write_request, (uv_stream_t *)client,
                     &response->write_buffer, 1, on_write);
  if (wr)
    on_write (&response->write_request, -wr);
}

static void
enqueue_response (struct http_client *client, struct http_response *response)
{
  free_request (client);
  response->client = client;
  bool idle = client->response_queue.next == &client->response_queue;
  list$ (ins) (&response->list_entry, client->response_queue.prev,
               &client->response_queue);
  if (idle)
    write_next (client);
}

static int
http_response_ex (struct http_client *client, char *header, char *extra,
                  byte const *content, usz length, bool body)
{
  usz hsiz = strlen (header);
  usz xsiz = extra ? strlen (extra) : 0;
  usz bufsiz = 16 + sizeof (H1) + hsiz + xsiz + sizeof (H1_CONNECTION)
               + umax$ (sizeof ("keep-alive"), sizeof ("close"))
               + sizeof (H1_CONTENT_LENGTH) + sizeof (quote$ (SIZE_MAX))
               + sizeof (H1_EOL) + (body ? length : 0);
  struct http_response *r = malloc (sizeof (*r) + bufsiz);
  if (!r)
    return HPE_USER;
  char *cur = r->write_buffer.base = r->buffer;
  r->keep_alive = llhttp_should_keep_alive (&client->parser);
  memcpy (cur, H1, sizeof (H1) - 1);
  cur += sizeof (H1) - 1;
  *cur++ = ' ';
  memcpy (cur, header, hsiz);
  cur += hsiz;
  cur += sprintf (cur, H1_CONNECTION, r->keep_alive ? "keep-alive" : "close");
  /* Content-Length always describes the selected representation, even when the
   * body is suppressed for HEAD. */
  cur += sprintf (cur, H1_CONTENT_LENGTH, length);
  if (xsiz)
    {
      memcpy (cur, extra, xsiz);
      cur += xsiz;
    }
  memcpy (cur, H1_EOL, sizeof (H1_EOL) - 1);
  cur += sizeof (H1_EOL) - 1;
  if (body && length)
    {
      memcpy (cur, content, length);
      cur += length;
    }
  r->write_buffer.len = cur - r->write_buffer.base;
  enqueue_response (client, r);
  return HPE_OK;
}

/* llhttp method -> ROUTE_* bit. Anything libr3 has no bit for maps to
 * ROUTE_OTHER so it can only ever match a ROUTE_ANY route. */
static int
route_method (llhttp_method_t method)
{
  switch (method)
    {
    case HTTP_GET:
      return ROUTE_GET;
    case HTTP_POST:
      return ROUTE_POST;
    case HTTP_PUT:
      return ROUTE_PUT;
    case HTTP_DELETE:
      return ROUTE_DELETE;
    case HTTP_PATCH:
      return ROUTE_PATCH;
    case HTTP_HEAD:
      return ROUTE_HEAD;
    case HTTP_OPTIONS:
      return ROUTE_OPTIONS;
    default:
      return ROUTE_OTHER;
    }
}

static usz
allow_header (int allowed, char *buffer, usz size)
{
  static struct
  {
    int bit;
    char const *name;
  } const table[] = {
    { ROUTE_GET, "GET" },       { ROUTE_HEAD, "HEAD" },
    { ROUTE_POST, "POST" },     { ROUTE_PUT, "PUT" },
    { ROUTE_PATCH, "PATCH" },   { ROUTE_DELETE, "DELETE" },
    { ROUTE_OPTIONS, "OPTIONS" },
  };
  char list[H1_ALLOW_MAX];
  char *cur = list;
  for (usz i = 0; i < countof (table); ++i)
    {
      if (!(allowed & table[i].bit))
        continue;
      if (cur != list)
        {
          *cur++ = ',';
          *cur++ = ' ';
        }
      usz len = strlen (table[i].name);
      memcpy (cur, table[i].name, len);
      cur += len;
    }
  *cur = '\0';
  if (cur == list)
    return 0;
  int written = snprintf (buffer, size, H1_ALLOW, list);
  return written > 0 && (usz)written < size ? (usz)written : 0;
}

char const *
http_capture (struct http_request const *request, char const *name,
              usz *length)
{
  if (!request || !name)
    return null;
  usz want = strlen (name);
  for (usz i = 0; i < request->capture_count; ++i)
    {
      struct router_capture const *capture = &request->capture[i];
      if (capture->name_length != want || !capture->name
          || memcmp (capture->name, name, want))
        continue;
      if (length)
        *length = capture->value_length;
      return capture->value;
    }
  if (length)
    *length = 0;
  return null;
}

int
http_route (int methods, char const *path, http_handler handler)
{
  if (!handler)
    return -1;
  if (!http_router && !(http_router = router_create (null)))
    return -1;
  return router_add (http_router, methods, path, (void *)handler);
}

/* Dispatch happens here rather than in on_url_complete because a handler
 * needs the method and body too, and llhttp only guarantees those by now. */
static int
on_message_complete (llhttp_t *parser)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);

  char *path = "";
  usz path_length = 0;
  if (client->url)
    {
      bsto *url = cstrbin$ (client->url);
      if (!url)
        return HPE_USER;
      client->url = url;
      path = (char *)url->store;
      path_length = url->size;
    }
  /* Route on the path only: strip query and fragment. */
  usz stop = strcspn (path, "?#");
  if (stop < path_length)
    path_length = stop;

  byte const *body = null;
  usz body_length = 0;
  if (client->body)
    {
      body = client->body->store;
      body_length = client->body->size;
    }

  auto method = (llhttp_method_t)llhttp_get_method (parser);
  struct http_loop *loop = client->loop;
  struct router_match *match = &loop->match;

  switch (router_lookup (loop->tree, route_method (method), path, path_length,
                         match))
    {
    case ROUTER_FOUND:;
      auto handler = (http_handler)match->data;
      struct http_request request = {
        .client = client,
        .method = method,
        .path = path,
        .path_length = path_length,
        .capture = match->capture,
        .capture_count = match->capture_count,
        .body = body,
        .body_length = body_length,
      };
      struct http_reply reply = { .status = HTTP_CODE_200 };
      handler (&request, &reply);
      char extra[sizeof (H1_CONTENT_TYPE) + 128];
      usz xlen = 0;
      if (reply.content_type)
        {
          int written = snprintf (extra, sizeof (extra), H1_CONTENT_TYPE,
                                  reply.content_type);
          if (written > 0 && (usz)written < sizeof (extra))
            xlen = written;
        }
      /* HEAD carries the same headers as GET but never a body. */
      int rc = http_response_ex (client, (char *)reply.status,
                                 xlen ? extra : null, reply.content,
                                 reply.length, method != HTTP_HEAD);
      if (reply.content_free)
        reply.content_free (reply.content);
      return rc;

    case ROUTER_METHOD:;
      char allow[sizeof (H1_ALLOW) + H1_ALLOW_MAX];
      usz alen = allow_header (match->allowed, allow, sizeof (allow));
      return http_response_ex (client, HTTP_CODE_405 H1_EOL,
                               alen ? allow : null, null, 0, true);

    case ROUTER_NOTFOUND:
      return http_response_ex (client, HTTP_CODE_404 H1_EOL, null,
                               (byte const *)"Not Found",
                               sizeof ("Not Found") - 1,
                               method != HTTP_HEAD);

    default:
      return HPE_USER;
    }
}

static int
on_body (llhttp_t *parser, char const *at, usz len)
{
  if (len == 0)
    return HPE_OK;
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  usz siz = client->body ? client->body->size : 0;
  if (!(client->body = rebin$ (client->body, siz + len)))
    return HPE_USER;
  memcpy (client->body->store + siz, at, len);
  return HPE_OK;
}

static int
header_compare (void const *u, void const *v)
{
  return strcasecmp ((*(struct http_header const **)u)->field,
                     (*(struct http_header const **)v)->field);
}

static int
on_headers_complete (llhttp_t *parser)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  if (!client->header)
    {
      client->header = calloc (1, sizeof (*client->header));
      return client->header ? HPE_OK : HPE_USER;
    }
  auto header = (struct http_header **)client->header->store;
  usz nr = client->header->size / sizeof (header[0]);
  if (nr > 0)
    {
      qsort (header, nr, sizeof (header[0]), header_compare);
      for (usz i = 0; i < nr; ++i)
        clogger (DEBUG, header[i]->field, "=", header[i]->value);
      for (usz i = 1; i < nr; ++i)
        if (header_compare (&header[i - 1], &header[i]) == 0)
          return HPE_USER;
    }
  return HPE_OK;
}

static struct http_header **
header_last (struct http_client *client)
{
  if (!client->header)
    return null;
  auto header = (struct http_header **)client->header->store;
  usz nr = client->header->size / sizeof (header[0]);
  if (nr == 0)
    return null;
  return &header[nr - 1];
}

static struct http_header **
header_alloc (struct http_client *client)
{
  usz cap = dynarr$ (0, 1);
  struct http_header *header = malloc (sizeof (*header) + cap);
  if (!header)
    return null;
  header->value = EMPTYCSTR;
  header->capacity = cap;
  header->size = 0;
  header->field[0] = '\0';
  usz nr = client->header
               ? client->header->size / sizeof (struct http_header *)
               : 0;
  usz siz = (nr + 1) * sizeof (header);
  bsto *bin = rebin$ (client->header, siz);
  if (!bin)
    {
      free (header);
      return null;
    }
  client->header = bin;
  auto slot = &((struct http_header **)bin->store)[nr];
  *slot = header;
  return slot;
}

static struct http_header *
header_reserve (struct http_header **slot, usz size)
{
  struct http_header *header = *slot;
  isz voff = header->value - header->field;
  usz cap = dynarr$ (header->capacity, size);
  header = realloc (header, sizeof (*header) + cap);
  if (!header)
    return null;
  header->capacity = cap;
  if (header->value != EMPTYCSTR)
    header->value = header->field + voff;
  return *slot = header;
}

static int
on_header_value (llhttp_t *parser, char const *at, usz len)
{
  if (len == 0)
    return HPE_OK;
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  auto slot = header_last (client);
  if (!slot)
    return -1;
  struct http_header *header = header_reserve (slot, (*slot)->size + len + 2);
  if (!header)
    return HPE_USER;
  if (header->value == EMPTYCSTR)
    header->value = header->field + ++header->size;
  memcpy (header->field + header->size, at, len);
  header->size += len;
  header->field[header->size] = '\0';
  return HPE_OK;
}

static int
on_header_field (llhttp_t *parser, char const *at, usz len)
{
  if (len == 0)
    return HPE_OK;
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  auto slot = header_last (client);
  if (!slot || (*slot)->value != EMPTYCSTR)
    slot = header_alloc (client);
  if (!slot)
    return HPE_USER;
  struct http_header *header = header_reserve (slot, (*slot)->size + len + 1);
  if (!header)
    return HPE_USER;
  memcpy (header->field + header->size, at, len);
  header->size += len;
  header->field[header->size] = '\0';
  return HPE_OK;
}

static int
on_url_complete (llhttp_t *parser)
{
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  bsto *url = cstrbin$ (client->url);
  if (!url)
    return HPE_USER;
  client->url = url;
  clogger (DEBUG, (char *)url->store);
  return HPE_OK;
}

static int
on_url (llhttp_t *parser, char const *at, usz len)
{
  if (len == 0)
    return HPE_OK;
  struct http_client *client
      = container_of (parser, struct http_client, parser);
  usz siz = client->url ? client->url->size : 0;
  if (!(client->url = rebin$ (client->url, siz + len)))
    return HPE_USER;
  memcpy (client->url->store + siz, at, len);
  return HPE_OK;
}

static void
on_read (uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf)
{
  auto client = (struct http_client *)stream;
  if (nread <= 0)
    {
      free (buf->base);
      if (nread < 0)
        close_client (client);
      return;
    }
  for (char *buffer = buf->base;;)
    {
      switch (llhttp_execute (&client->parser, buffer, nread))
        {
        case HPE_OK:;
          break;
        case HPE_PAUSED:;
          close_client (client);
          break;
        case HPE_PAUSED_UPGRADE:;
          llhttp_resume_after_upgrade (&client->parser);
          auto off = llhttp_get_error_pos (&client->parser) - buffer;
          if (off < nread)
            {
              buffer += off;
              nread -= off;
              continue;
            }
          break;
        default:;
          uv_read_stop (stream);
          struct http_response *response = malloc (sizeof (*response));
          if (response)
            {
              response->keep_alive = false;
              response->write_buffer.base = H1_RESPONSE_400;
              response->write_buffer.len = strlen (H1_RESPONSE_400);
              enqueue_response (client, response);
            }
          else
            close_client (client);
          break;
        }
      break;
    }
  free (buf->base);
}

static void
on_read_alloc (uv_handle_t *handle, size_t siz, uv_buf_t *buf)
{
  buf->base = null;
  while (siz && !(buf->base = malloc (siz)))
    siz >>= 1;
  buf->len = siz;
}

static void
on_connection (uv_stream_t *srv, int status)
{
  if (status)
    return;
  struct http_client *client = calloc (1, sizeof (*client));
  if (!client)
    {
      uv_tcp_t *closer = null;
      while (!closer)
        {
#if _P_PLATFORM_ == (_P_UNIX_ + 0)
          sched_yield ();
#elif _P_PLATFORM_ == (_P_WINDOWS_ + 0)
          SwitchToThread ();
#endif
          closer = malloc (sizeof (*closer));
        }
      uv_tcp_init (srv->loop, closer);
      if (uv_accept (srv, (uv_stream_t *)closer))
        return uv_close ((uv_handle_t *)closer, (uv_close_cb)free);
      uv_tcp_close_reset (closer, (uv_close_cb)free);
      return;
    }
  uv_tcp_init (srv->loop, &client->tcp_handle);
  if (uv_accept (srv, (uv_stream_t *)client))
    return uv_close ((uv_handle_t *)client, (uv_close_cb)free);
  client->loop = srv->data;
  llhttp_init (&client->parser, HTTP_REQUEST, &llhttp_settings);
  client->response_queue.next = client->response_queue.prev
      = &client->response_queue;
  uv_read_start ((uv_stream_t *)client, on_read_alloc, on_read);
}

static void
init_static ()
{
  static bool inited;
  if (inited)
    return;
  llhttp_settings_init (&llhttp_settings);
  llhttp_settings.on_url = on_url;
  llhttp_settings.on_url_complete = on_url_complete;
  llhttp_settings.on_header_field = on_header_field;
  llhttp_settings.on_header_value = on_header_value;
  llhttp_settings.on_headers_complete = on_headers_complete;
  llhttp_settings.on_body = on_body;
  llhttp_settings.on_message_complete = on_message_complete;
  inited = true;

  (void)H1_RESPONSE_404;
}

static int
serve (uv_loop_t *loop, struct sockaddr const *addr, unsigned flags)
{
  /* Compile a tree per loop: sharing one across threads would race on the
   * PCRE2 match data libr3 keeps inside each node. */
  char *errstr = null;
  struct http_loop context = { 0 };
  if (!(context.tree = router_compile (http_router, &errstr)))
    {
      cerr ("route compilation failed:", errstr ? errstr : "out of memory");
      free (errstr);
      return 1;
    }

  uv_tcp_t server;
  uv_tcp_init_ex (loop, &server, addr->sa_family);
  server.data = &context;
  if (addr->sa_family == AF_INET6)
    {
      uv_os_fd_t fd;
      if (!uv_fileno ((uv_handle_t *)&server, &fd))
        setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){ 0 }, sizeof (int));
    }
  int result;
  if (uv_tcp_bind (&server, addr, flags)
      || uv_listen ((uv_stream_t *)&server, 16384, on_connection))
    {
      uv_close ((uv_handle_t *)&server, null);
      uv_run (loop, UV_RUN_DEFAULT);
      result = 1;
    }
  else
    result = uv_run (loop, UV_RUN_DEFAULT);
  router_match_release (&context.match);
  router_tree_free (context.tree);
  return result;
}

struct worker
{
  pthread_t thread;
  uv_loop_t loop;
  struct sockaddr const *addr;
  int result;
};

static void *
worker_main (void *arg)
{
  struct worker *w = arg;
  w->result = serve (&w->loop, w->addr, UV_TCP_REUSEPORT);
  return null;
}

int
http_listen (struct sockaddr const *addr, long threads)
{
  signal (SIGPIPE, SIG_IGN);
  init_static ();
  /* A server with no registered routes is legal: every request gets a 404. */
  if (!http_router && !(http_router = router_create (null)))
    return 1;
  if (threads <= 0)
    threads = uv_available_parallelism () + threads;
  if (threads <= 1)
    return serve (uv_default_loop (), addr, 0);
  struct worker *w = calloc (threads, sizeof (*w));
  if (!w)
    return 1;
  unsigned started;
  for (started = 0; started < threads; ++started)
    {
      if (uv_loop_init (&w[started].loop))
        break;
      w[started].addr = addr;
      if (pthread_create (&w[started].thread, null, worker_main, &w[started]))
        {
          uv_loop_close (&w[started].loop);
          break;
        }
    }
  if (started == 0)
    {
      free (w);
      return 1;
    }
  int result = 0;
  for (unsigned i = 0; i < started; ++i)
    {
      pthread_join (w[i].thread, null);
      uv_loop_close (&w[i].loop);
      if (w[i].result)
        result = w[i].result;
    }
  free (w);
  return result;
}
