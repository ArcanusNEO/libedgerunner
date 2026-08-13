#pragma once
#ifndef _H_ROUTER_
#define _H_ROUTER_ 1

#include "cmacs.h"
#include <grimoire.h>

/* Method bits. These intentionally mirror libr3's METHOD_* values so that no
 * translation is needed internally; router.c static_asserts the equality.
 * ROUTE_OTHER covers every HTTP method libr3 has no bit for (CONNECT, TRACE,
 * WebDAV verbs, ...) so that such a request can never satisfy a route that was
 * registered for a specific method. */
#define ROUTE_ANY 0
#define ROUTE_GET (2 << 0)
#define ROUTE_POST (2 << 1)
#define ROUTE_PUT (2 << 2)
#define ROUTE_DELETE (2 << 3)
#define ROUTE_PATCH (2 << 4)
#define ROUTE_HEAD (2 << 5)
#define ROUTE_OPTIONS (2 << 6)
#define ROUTE_OTHER (2 << 19)
#define ROUTE_ALL                                                             \
  (ROUTE_GET | ROUTE_POST | ROUTE_PUT | ROUTE_DELETE | ROUTE_PATCH            \
   | ROUTE_HEAD | ROUTE_OPTIONS | ROUTE_OTHER)

/* Route registry: holds the route table in a plain, immutable-after-setup
 * form. It owns copies of the path patterns, so every tree compiled from it
 * may borrow those strings. Must outlive every tree compiled from it. */
struct router;

/* A compiled radix tree. Matching mutates per-node PCRE2 match data, so a tree
 * must not be shared between threads: compile one per event loop. */
struct router_tree;

struct router_capture
{
  char const *name;
  usz name_length;
  /* Points into the path buffer handed to router_lookup, not a copy. */
  char const *value;
  usz value_length;
};

struct router_match
{
  /* Data of the matched route; only valid on ROUTER_FOUND. */
  void *data;
  /* OR of the method masks registered on the matched path. Valid on
   * ROUTER_FOUND and ROUTER_METHOD, so a 405 can advertise Allow. */
  int allowed;
  struct router_capture *capture;
  usz capture_count;

  /* Internal scratch, reused across lookups. Zero-initialise once, then
   * release with router_match_release. */
  usz capture_capacity;
  struct
  {
    void *entries;
    unsigned int size;
    unsigned int capacity;
  } tokens;
};

enum
{
  ROUTER_FOUND = 0,
  ROUTER_NOTFOUND = 1, /* no route for this path   -> 404 */
  ROUTER_METHOD = 2,   /* path exists, method does not -> 405 */
  ROUTER_ERROR = -1
};

struct router *router_create (void (*data_free) (void *));
void router_free (struct router *router);

/* Registers path (r3 syntax: "/user/{id}", "/file/{path:.*}") for the given
 * method mask. ROUTE_ANY matches every method. Returns 0 on success. */
int router_add (struct router *router, int methods, char const *path,
                void *data);

usz router_count (struct router const *router);

/* Compiles a private tree. On failure returns null and, when errstr is not
 * null, stores a malloc'd message there. */
struct router_tree *router_compile (struct router const *router,
                                    char **errstr);
void router_tree_free (struct router_tree *tree);

int router_lookup (struct router_tree *tree, int method, char const *path,
                   usz path_length, struct router_match *match);
void router_match_release (struct router_match *match);

#endif /* _H_ROUTER_ */
