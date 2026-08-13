#include "router.h"

#include <r3/r3.h>

/* The public ROUTE_* bits are meant to be usable wherever libr3's METHOD_* are
 * expected. Keep the two in lockstep. */
static_assert (ROUTE_GET == METHOD_GET, "ROUTE_GET drifted from METHOD_GET");
static_assert (ROUTE_POST == METHOD_POST, "ROUTE_POST drifted");
static_assert (ROUTE_PUT == METHOD_PUT, "ROUTE_PUT drifted");
static_assert (ROUTE_DELETE == METHOD_DELETE, "ROUTE_DELETE drifted");
static_assert (ROUTE_PATCH == METHOD_PATCH, "ROUTE_PATCH drifted");
static_assert (ROUTE_HEAD == METHOD_HEAD, "ROUTE_HEAD drifted");
static_assert (ROUTE_OPTIONS == METHOD_OPTIONS, "ROUTE_OPTIONS drifted");

struct route_handler
{
  int methods;
  void *data;
};

/* One entry per distinct path pattern. Several handlers may share a path when
 * they were registered for different methods. */
struct route_path
{
  char *path;
  usz length;
  int allowed;
  struct route_handler *handler;
  usz count;
  usz capacity;
};

struct router
{
  struct route_path *path;
  usz count;
  usz capacity;
  void (*data_free) (void *);
};

struct router_tree
{
  R3Node *root;
};

struct router *
router_create (void (*data_free) (void *))
{
  struct router *router = calloc (1, sizeof (*router));
  if (!router)
    return null;
  router->data_free = data_free;
  return router;
}

void
router_free (struct router *router)
{
  if (!router)
    return;
  for (usz i = 0; i < router->count; ++i)
    {
      struct route_path *path = &router->path[i];
      if (router->data_free)
        for (usz j = 0; j < path->count; ++j)
          router->data_free (path->handler[j].data);
      free (path->handler);
      free (path->path);
    }
  free (router->path);
  free (router);
}

static struct route_path *
path_find (struct router *router, char const *path, usz length)
{
  for (usz i = 0; i < router->count; ++i)
    if (router->path[i].length == length
        && memcmp (router->path[i].path, path, length) == 0)
      return &router->path[i];
  return null;
}

static struct route_path *
path_alloc (struct router *router, char const *path, usz length)
{
  usz cap = dynarr$ (router->capacity * sizeof (*router->path),
                     (router->count + 1) * sizeof (*router->path));
  if (cap != router->capacity * sizeof (*router->path))
    {
      struct route_path *grown = realloc (router->path, cap);
      if (!grown)
        return null;
      router->path = grown;
      router->capacity = cap / sizeof (*router->path);
    }
  struct route_path *slot = &router->path[router->count];
  *slot = (struct route_path){ 0 };
  /* Owned copy: libr3 borrows the pattern for both routes and slugs. */
  if (!(slot->path = malloc (length + 1)))
    return null;
  memcpy (slot->path, path, length);
  slot->path[length] = '\0';
  slot->length = length;
  ++router->count;
  return slot;
}

int
router_add (struct router *router, int methods, char const *path, void *data)
{
  if (!router || !path)
    return -1;
  usz length = strlen (path);
  if (length == 0)
    return -1;
  bool fresh = false;
  struct route_path *slot = path_find (router, path, length);
  if (!slot)
    {
      if (!(slot = path_alloc (router, path, length)))
        return -1;
      fresh = true;
    }
  usz cap = dynarr$ (slot->capacity * sizeof (*slot->handler),
                     (slot->count + 1) * sizeof (*slot->handler));
  if (cap != slot->capacity * sizeof (*slot->handler))
    {
      struct route_handler *grown = realloc (slot->handler, cap);
      if (!grown)
        {
          if (fresh)
            {
              free (slot->path);
              --router->count;
            }
          return -1;
        }
      slot->handler = grown;
      slot->capacity = cap / sizeof (*slot->handler);
    }
  slot->handler[slot->count++]
      = (struct route_handler){ .methods = methods, .data = data };
  slot->allowed |= methods == ROUTE_ANY ? ROUTE_ALL : methods;
  return 0;
}

usz
router_count (struct router const *router)
{
  return router ? router->count : 0;
}

struct router_tree *
router_compile (struct router const *router, char **errstr)
{
  if (errstr)
    *errstr = null;
  if (!router)
    return null;
  struct router_tree *tree = calloc (1, sizeof (*tree));
  if (!tree)
    return null;
  /* r3_tree_create aborts rather than returning null when out of memory. */
  tree->root = r3_tree_create (router->count ? router->count : 1);
  if (!tree->root)
    {
      free (tree);
      return null;
    }
  for (usz i = 0; i < router->count; ++i)
    {
      struct route_path *path = &router->path[i];
      /* Method filtering happens in router_lookup so that a path known under
       * a different method yields 405 instead of 404. Hence method 0 here. */
      char *err = null;
      R3Route *route = r3_tree_insert_routel_ex (
          tree->root, 0, path->path, path->length, path, &err);
      if (!route)
        {
          if (errstr)
            *errstr = err;
          else
            free (err);
          router_tree_free (tree);
          return null;
        }
    }
  char *err = null;
  if (r3_tree_compile (tree->root, &err))
    {
      if (errstr)
        *errstr = err;
      else
        free (err);
      router_tree_free (tree);
      return null;
    }
  return tree;
}

void
router_tree_free (struct router_tree *tree)
{
  if (!tree)
    return;
  if (tree->root)
    r3_tree_free (tree->root);
  free (tree);
}

static int
capture_reserve (struct router_match *match, usz count)
{
  if (count <= match->capture_capacity)
    return 0;
  usz cap = dynarr$ (match->capture_capacity * sizeof (*match->capture),
                     count * sizeof (*match->capture));
  struct router_capture *grown = realloc (match->capture, cap);
  if (!grown)
    return -1;
  match->capture = grown;
  match->capture_capacity = cap / sizeof (*match->capture);
  return 0;
}

int
router_lookup (struct router_tree *tree, int method, char const *path,
               usz path_length, struct router_match *match)
{
  if (!tree || !tree->root || !path || !match)
    return ROUTER_ERROR;
  match->data = null;
  match->allowed = 0;
  match->capture_count = 0;
  if (path_length == 0 || path_length > UINT_MAX)
    return ROUTER_NOTFOUND;

  /* Borrow the reusable token vector so repeated lookups do not re-allocate.
   * match_entry is used on the stack: r3 never keeps a reference to it. */
  match_entry entry = { 0 };
  entry.vars.tokens.entries = match->tokens.entries;
  entry.vars.tokens.capacity = match->tokens.capacity;
  entry.vars.tokens.size = 0;
  entry.path = r3_iovec_init (path, path_length);
  entry.request_method = method;

  R3Route *route = r3_tree_match_route (tree->root, &entry);

  match->tokens.entries = entry.vars.tokens.entries;
  match->tokens.capacity = entry.vars.tokens.capacity;
  match->tokens.size = entry.vars.tokens.size;

  if (!route || !route->data)
    return ROUTER_NOTFOUND;

  auto found = (struct route_path *)route->data;
  match->allowed = found->allowed;
  for (usz i = 0; i < found->count; ++i)
    {
      struct route_handler *handler = &found->handler[i];
      if (handler->methods != ROUTE_ANY && !(handler->methods & method))
        continue;
      match->data = handler->data;

      usz count = entry.vars.tokens.size;
      if (count && capture_reserve (match, count))
        return ROUTER_ERROR;
      auto token = (r3_iovec_t *)entry.vars.tokens.entries;
      auto slug = entry.vars.slugs.entries;
      usz named = entry.vars.slugs.size;
      for (usz j = 0; j < count; ++j)
        {
          match->capture[j].value = token[j].base;
          match->capture[j].value_length = token[j].len;
          /* Slug names are only known for routes that declared them. */
          match->capture[j].name = j < named ? slug[j].base : null;
          match->capture[j].name_length = j < named ? slug[j].len : 0;
        }
      match->capture_count = count;
      return ROUTER_FOUND;
    }
  return ROUTER_METHOD;
}

void
router_match_release (struct router_match *match)
{
  if (!match)
    return;
  free (match->capture);
  match->capture = null;
  match->capture_capacity = 0;
  match->capture_count = 0;
  free (match->tokens.entries);
  match->tokens.entries = null;
  match->tokens.size = match->tokens.capacity = 0;
}
