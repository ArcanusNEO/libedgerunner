#include "http.h"

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
  long threads = argc == 4 ? atol (argv[3]) : 0;
  return http_listen ((struct sockaddr *)&addr, threads);
usage:
  cerr ("usage:", argv[0], "<host> <port> [threads]");
  return 1;
}
