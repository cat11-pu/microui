#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/microui.h"

/*
** The library promises to stay inside its fixed-size context. Count every
** dynamic allocation made by microui.c itself; the test harness still uses
** the normal allocator.
*/
typedef union {
  struct { size_t size; } h;
  long double align_ld;
  void *align_ptr;
} TestAllocHeader;

static long g_alloc_calls;
static long g_free_calls;
static long g_live_bytes;

static void *test_malloc(size_t size) {
  TestAllocHeader *h = malloc(sizeof(*h) + size);
  if (!h) { return NULL; }
  h->h.size = size;
  g_alloc_calls++;
  g_live_bytes += (long) size;
  return h + 1;
}

static void test_free(void *ptr) {
  TestAllocHeader *h;
  if (!ptr) { return; }
  h = ((TestAllocHeader *) ptr) - 1;
  g_live_bytes -= (long) h->h.size;
  g_free_calls++;
  free(h);
}

static void *test_calloc(size_t count, size_t size) {
  size_t total;
  void *ptr;
  if (size && count > (size_t) -1 / size) { return NULL; }
  total = count * size;
  ptr = test_malloc(total);
  if (ptr) { memset(ptr, 0, total); }
  return ptr;
}

static void *test_realloc(void *ptr, size_t size) {
  TestAllocHeader *h;
  size_t old_size;
  if (!ptr) { return test_malloc(size); }
  if (!size) {
    test_free(ptr);
    return NULL;
  }
  h = ((TestAllocHeader *) ptr) - 1;
  old_size = h->h.size;
  h = realloc(h, sizeof(*h) + size);
  if (!h) { return NULL; }
  h->h.size = size;
  g_live_bytes += (long) size - (long) old_size;
  return h + 1;
}

#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../src/microui.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

#define ARRAY_LEN(a) ((int) (sizeof(a) / sizeof((a)[0])))
#define MAX_NODES 160
#define MAX_KIDS 8
#define MAX_DEPTH 4
#define FRAMES 4
#define MAX_UNIQUE_FAILURES 16
#define EVAL_TIMEOUT_SECONDS 8
#define SHRINK_LIMIT 700

enum NodeKind {
  N_WINDOW,
  N_PANEL,
  N_CLIP,
  N_SCROLL,
  N_HIDDEN,
  N_LABEL,
  N_BUTTON,
  N_CHECKBOX,
  N_SLIDER,
  N_TEXTBOX,
  N_NONE
};

enum FailureKind {
  F_PASS,
  F_CRASH,
  F_TIMEOUT,
  F_LEAK,
  F_STACK,
  F_NONDET,
  F_INTERNAL
};

typedef struct Node {
  int kind;
  int parent;
  int kids[MAX_KIDS];
  int nkids;
  int width;
  int height;
  int opt;
  int phase;
  int clip_dx;
  int clip_dy;
  int clip_w;
  int clip_h;
  int checked;
  mu_Real value;
  char label[24];
  char text[32];
} Node;

typedef struct Case {
  uint64_t seed;
  uint64_t rng;
  int used;
  int root;
  int feature_cursor;
  int rect_x;
  int rect_y;
  int rect_w;
  int rect_h;
  Node nodes[MAX_NODES];
} Case;

typedef struct FrameTrace {
  uint64_t command_hash;
  uint64_t state_hash;
  int command_count;
  int stack_failure;
  int stack_depth;
} FrameTrace;

typedef struct RunTrace {
  FrameTrace frames[FRAMES];
  long alloc_calls;
  long free_calls;
  long live_bytes;
  long command_total;
  int max_scroll_y;
} RunTrace;

typedef struct EvalResult {
  int kind;
  int signal;
  int frame;
  int stack_index;
  int tree_nodes;
  long alloc_calls;
  long alloc_live;
  uint64_t hash_a;
  uint64_t hash_b;
  int count_a;
  int count_b;
  int command_total;
  int max_scroll_y;
  char detail[256];
} EvalResult;

typedef struct Runtime {
  mu_Container *scroll_container;
  int command_count;
} Runtime;

static const int feature_kinds[] = { N_PANEL, N_CLIP, N_SCROLL, N_HIDDEN };
static const char *failure_names[] = {
  "pass", "crash", "timeout", "leak", "stack", "nondeterminism", "internal"
};

static uint64_t rng_u64(uint64_t *state) {
  uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}

static int rng_int(uint64_t *state, int low, int high) {
  uint64_t span;
  if (high <= low) { return low; }
  span = (uint64_t) (high - low + 1);
  return low + (int) (rng_u64(state) % span);
}

static int node_is_feature(int kind) {
  return kind >= N_PANEL && kind <= N_HIDDEN;
}

static int add_node(Case *c, int kind, int parent) {
  Node *n;
  int idx;
  if (c->used >= MAX_NODES) { return -1; }
  idx = c->used++;
  n = &c->nodes[idx];
  memset(n, 0, sizeof(*n));
  n->kind = kind;
  n->parent = parent;
  n->width = 0;
  n->height = 0;
  n->clip_w = 80;
  n->clip_h = 50;
  n->phase = rng_int(&c->rng, 0, 3);
  snprintf(n->label, sizeof(n->label), "x%03d", idx);
  snprintf(n->text, sizeof(n->text), "t%03d", idx);

  if (node_is_feature(kind)) {
    n->width = rng_int(&c->rng, 64, 180);
    n->height = rng_int(&c->rng, 42, 108);
  } else {
    int w = rng_int(&c->rng, 0, 99);
    int h = rng_int(&c->rng, 0, 99);
    n->width = w < 18 ? -1 : (w < 28 ? 0 : rng_int(&c->rng, 12, 110));
    n->height = h < 12 ? 0 : rng_int(&c->rng, 10, 34);
  }

  if (kind == N_PANEL) {
    n->opt = (rng_int(&c->rng, 0, 4) == 0) ? MU_OPT_NOSCROLL : 0;
  } else if (kind == N_SCROLL) {
    n->height = rng_int(&c->rng, 28, 46);
    n->opt = 0;
  } else if (kind == N_CLIP) {
    n->clip_dx = rng_int(&c->rng, 0, 24);
    n->clip_dy = rng_int(&c->rng, 0, 18);
    n->clip_w = rng_int(&c->rng, 20, 120);
    n->clip_h = rng_int(&c->rng, 16, 80);
  } else if (kind == N_SLIDER) {
    n->value = (mu_Real) rng_int(&c->rng, 0, 100);
  } else if (kind == N_CHECKBOX) {
    n->checked = rng_int(&c->rng, 0, 1);
  } else if (kind == N_BUTTON) {
    n->opt = (rng_int(&c->rng, 0, 5) == 0) ? MU_OPT_NOINTERACT : 0;
  }
  return idx;
}

static void add_kid(Case *c, int parent, int kid) {
  Node *p = &c->nodes[parent];
  if (kid < 0 || p->nkids >= MAX_KIDS) { return; }
  p->kids[p->nkids++] = kid;
  c->nodes[kid].parent = parent;
}

static void gen_subtree(Case *c, int parent, int depth) {
  int count = rng_int(&c->rng, 2, 5);
  int i;
  for (i = 0; i < count; i++) {
    int kind;
    int child;
    if (c->used >= MAX_NODES - 1) { return; }
    if (depth < MAX_DEPTH && rng_int(&c->rng, 0, 99) < 66) {
      kind = feature_kinds[c->feature_cursor++ % ARRAY_LEN(feature_kinds)];
    } else {
      kind = N_LABEL + rng_int(&c->rng, 0, 4);
    }
    child = add_node(c, kind, parent);
    if (child < 0) { return; }
    add_kid(c, parent, child);
    if (node_is_feature(kind)) {
      gen_subtree(c, child, depth + 1);
    }
  }
}

static void gen_case(Case *c, uint64_t seed) {
  int rotation;
  int i;
  memset(c, 0, sizeof(*c));
  c->seed = seed;
  c->rng = seed ^ UINT64_C(0xa0761d6478bd642f);
  c->rect_x = rng_int(&c->rng, 8, 48);
  c->rect_y = rng_int(&c->rng, 8, 48);
  c->rect_w = rng_int(&c->rng, 300, 520);
  c->rect_h = rng_int(&c->rng, 220, 400);

  c->root = add_node(c, N_WINDOW, -1);
  c->nodes[c->root].label[0] = '\0';
  c->nodes[c->root].opt = 0;
  if (rng_int(&c->rng, 0, 1)) { c->nodes[c->root].opt |= MU_OPT_NORESIZE; }
  if (rng_int(&c->rng, 0, 1)) { c->nodes[c->root].opt |= MU_OPT_NOCLOSE; }
  if (rng_int(&c->rng, 0, 2) == 0) { c->nodes[c->root].opt |= MU_OPT_NOSCROLL; }
  if (rng_int(&c->rng, 0, 3) == 0) { c->nodes[c->root].opt |= MU_OPT_NOTITLE; }

  /*
  ** Guarantee all four stateful features. Their order rotates by seed, while
  ** deeper feature nodes continue the same cycle so the categories alternate.
  */
  rotation = (int) (seed % ARRAY_LEN(feature_kinds));
  c->feature_cursor = rotation;
  for (i = 0; i < ARRAY_LEN(feature_kinds); i++) {
    int kind = feature_kinds[(rotation + i) % ARRAY_LEN(feature_kinds)];
    int child = add_node(c, kind, c->root);
    if (child < 0) { break; }
    add_kid(c, c->root, child);
    gen_subtree(c, child, 1);
    if (rng_int(&c->rng, 0, 2) == 0 && c->nodes[c->root].nkids < MAX_KIDS) {
      int leaf = add_node(c, N_LABEL + rng_int(&c->rng, 0, 4), c->root);
      if (leaf >= 0) { add_kid(c, c->root, leaf); }
    }
  }
  while (c->nodes[c->root].nkids < MAX_KIDS && c->used < MAX_NODES / 2 &&
         rng_int(&c->rng, 0, 2) == 0) {
    int leaf = add_node(c, N_LABEL + rng_int(&c->rng, 0, 4), c->root);
    if (leaf < 0) { break; }
    add_kid(c, c->root, leaf);
  }

  /* Put one scroll feature first so the wheel path is always on screen. */
  for (i = 1; i < c->nodes[c->root].nkids; i++) {
    int kid = c->nodes[c->root].kids[i];
    if (c->nodes[kid].kind == N_SCROLL) {
      int first = c->nodes[c->root].kids[0];
      c->nodes[c->root].kids[0] = kid;
      c->nodes[c->root].kids[i] = first;
      break;
    }
  }
}

static int hidden_visible(const Node *n, int frame) {
  return ((frame + n->phase) & 1) == 0;
}

static void render_children(mu_Context *ctx, Case *c, int idx, int frame,
  Runtime *rt);

static void begin_isolated_layout(mu_Context *ctx) {
  mu_Rect cell = mu_layout_next(ctx);
  mu_layout_set_next(ctx, cell, 0);
  mu_layout_begin_column(ctx);
}

static void render_node(mu_Context *ctx, Case *c, int idx, int frame,
  Runtime *rt)
{
  Node *n = &c->nodes[idx];
  switch (n->kind) {
  case N_PANEL:
    mu_begin_panel_ex(ctx, n->label, n->opt);
    render_children(ctx, c, idx, frame, rt);
    mu_end_panel(ctx);
    break;
  case N_SCROLL:
    mu_begin_panel_ex(ctx, n->label, 0);
    if (!rt->scroll_container) {
      rt->scroll_container = mu_get_current_container(ctx);
    }
    render_children(ctx, c, idx, frame, rt);
    mu_end_panel(ctx);
    break;
  case N_CLIP: {
    mu_Rect cell;
    mu_Rect clip;
    begin_isolated_layout(ctx);
    cell = ctx->last_rect;
    clip = mu_rect(
      cell.x + n->clip_dx,
      cell.y + n->clip_dy,
      n->clip_w,
      n->clip_h);
    mu_push_clip_rect(ctx, clip);
    render_children(ctx, c, idx, frame, rt);
    mu_pop_clip_rect(ctx);
    mu_layout_end_column(ctx);
    break;
  }
  case N_HIDDEN:
    if (!hidden_visible(n, frame)) { return; }
    begin_isolated_layout(ctx);
    render_children(ctx, c, idx, frame, rt);
    mu_layout_end_column(ctx);
    break;
  case N_LABEL:
    mu_label(ctx, n->label);
    break;
  case N_BUTTON:
    mu_button_ex(ctx, n->label,
      (idx % 7 == 0) ? MU_ICON_CHECK : 0, n->opt);
    break;
  case N_CHECKBOX:
    mu_checkbox(ctx, n->label, &n->checked);
    break;
  case N_SLIDER:
    mu_slider_ex(ctx, &n->value, 0, 100, 0, MU_SLIDER_FMT, 0);
    break;
  case N_TEXTBOX:
    mu_textbox_ex(ctx, n->text, (int) sizeof(n->text), 0);
    break;
  default:
    break;
  }
}

static void render_children(mu_Context *ctx, Case *c, int idx, int frame,
  Runtime *rt)
{
  Node *n = &c->nodes[idx];
  int i;
  for (i = 0; i < n->nkids; i++) {
    int kid = n->kids[i];
    Node *child = &c->nodes[kid];
    mu_layout_row(ctx, 1, &child->width, child->height);
    render_node(ctx, c, kid, frame, rt);
  }
}

static void render_case(mu_Context *ctx, Case *c, int frame, Runtime *rt) {
  Node *root = &c->nodes[c->root];
  if (mu_begin_window_ex(ctx, "root",
      mu_rect(c->rect_x, c->rect_y, c->rect_w, c->rect_h), root->opt)) {
    render_children(ctx, c, c->root, frame, rt);
    mu_end_window(ctx);
  }
}

static int test_text_width(mu_Font font, const char *str, int len) {
  int i;
  int width = 0;
  (void) font;
  if (len < 0) { len = (int) strlen(str); }
  for (i = 0; i < len; i++) {
    width += ((unsigned char) str[i] % 5) + 2;
  }
  return width;
}

static int test_text_height(mu_Font font) {
  (void) font;
  return 12;
}

static uint64_t mix_u64(uint64_t h, uint64_t value) {
  int i;
  for (i = 0; i < 8; i++) {
    h ^= (unsigned char) (value & 0xff);
    h *= UINT64_C(1099511628211);
    value >>= 8;
  }
  return h;
}

static uint64_t mix_i32(uint64_t h, int value) {
  return mix_u64(h, (uint64_t) (uint32_t) value);
}

static uint64_t mix_bytes(uint64_t h, const void *data, size_t size) {
  const unsigned char *p = data;
  size_t i;
  for (i = 0; i < size; i++) {
    h ^= p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static uint64_t mix_rect(uint64_t h, mu_Rect rect) {
  h = mix_i32(h, rect.x);
  h = mix_i32(h, rect.y);
  h = mix_i32(h, rect.w);
  h = mix_i32(h, rect.h);
  return h;
}

static uint64_t mix_color(uint64_t h, mu_Color color) {
  h = mix_i32(h, color.r);
  h = mix_i32(h, color.g);
  h = mix_i32(h, color.b);
  h = mix_i32(h, color.a);
  return h;
}

static int check_stack_balance(mu_Context *ctx, int *index) {
  if (ctx->container_stack.idx != 0) { *index = 0; return 0; }
  if (ctx->clip_stack.idx != 0) { *index = 1; return 0; }
  if (ctx->id_stack.idx != 0) { *index = 2; return 0; }
  if (ctx->layout_stack.idx != 0) { *index = 3; return 0; }
  return 1;
}

static void digest_frame(mu_Context *ctx, FrameTrace *trace) {
  mu_Command *cmd = NULL;
  uint64_t command_hash = UINT64_C(1469598103934665603);
  uint64_t state_hash = UINT64_C(1469598103934665603);
  int command_count = 0;
  int i;

  while (mu_next_command(ctx, &cmd)) {
    command_hash = mix_i32(command_hash, cmd->base.type);
    command_hash = mix_i32(command_hash, cmd->base.size);
    switch (cmd->type) {
    case MU_COMMAND_JUMP:
      break;
    case MU_COMMAND_CLIP:
      command_hash = mix_rect(command_hash, cmd->clip.rect);
      break;
    case MU_COMMAND_RECT:
      command_hash = mix_rect(command_hash, cmd->rect.rect);
      command_hash = mix_color(command_hash, cmd->rect.color);
      break;
    case MU_COMMAND_TEXT: {
      int len = cmd->base.size - (int) sizeof(mu_TextCommand);
      command_hash = mix_i32(command_hash, cmd->text.pos.x);
      command_hash = mix_i32(command_hash, cmd->text.pos.y);
      command_hash = mix_color(command_hash, cmd->text.color);
      command_hash = mix_i32(command_hash, len);
      if (len > 0) {
        command_hash = mix_bytes(command_hash, cmd->text.str, (size_t) len);
      }
      break;
    }
    case MU_COMMAND_ICON:
      command_hash = mix_rect(command_hash, cmd->icon.rect);
      command_hash = mix_i32(command_hash, cmd->icon.id);
      command_hash = mix_color(command_hash, cmd->icon.color);
      break;
    default:
      command_hash = mix_i32(command_hash, -1);
      break;
    }
    command_count++;
  }

  state_hash = mix_i32(state_hash, command_count);
  state_hash = mix_i32(state_hash, ctx->root_list.idx);
  state_hash = mix_i32(state_hash, ctx->last_zindex);
  for (i = 0; i < MU_CONTAINERPOOL_SIZE; i++) {
    mu_Container *cnt = &ctx->containers[i];
    mu_PoolItem *item = &ctx->container_pool[i];
    state_hash = mix_i32(state_hash, item->last_update);
    state_hash = mix_u64(state_hash, item->id);
    state_hash = mix_rect(state_hash, cnt->rect);
    state_hash = mix_rect(state_hash, cnt->body);
    state_hash = mix_i32(state_hash, cnt->content_size.x);
    state_hash = mix_i32(state_hash, cnt->content_size.y);
    state_hash = mix_i32(state_hash, cnt->scroll.x);
    state_hash = mix_i32(state_hash, cnt->scroll.y);
    state_hash = mix_i32(state_hash, cnt->zindex);
    state_hash = mix_i32(state_hash, cnt->open);
  }

  trace->command_hash = command_hash;
  trace->state_hash = state_hash;
  trace->command_count = command_count;
}

static void prepare_scroll_input(mu_Context *ctx, Runtime *rt, int frame) {
  if (frame > 0 && rt->scroll_container) {
    mu_Rect body = rt->scroll_container->body;
    if (body.w > 0 && body.h > 0) {
      mu_input_mousemove(ctx, body.x + body.w / 2, body.y + body.h / 2);
    }
    mu_input_scroll(ctx, 0, 7 + frame * 5);
  }
}

static void run_trace(Case *c, RunTrace *out) {
  mu_Context *ctx = malloc(sizeof(*ctx));
  Runtime rt;
  long calls_before = g_alloc_calls;
  long frees_before = g_free_calls;
  long live_before = g_live_bytes;
  int frame;
  memset(out, 0, sizeof(*out));
  memset(&rt, 0, sizeof(rt));
  if (!ctx) {
    out->frames[0].stack_failure = 1;
    out->frames[0].stack_depth = -1;
    return;
  }
  mu_init(ctx);
  ctx->text_width = test_text_width;
  ctx->text_height = test_text_height;

  for (frame = 0; frame < FRAMES; frame++) {
    prepare_scroll_input(ctx, &rt, frame);
    mu_begin(ctx);
    render_case(ctx, c, frame, &rt);
    mu_end(ctx);
    out->frames[frame].stack_failure =
      !check_stack_balance(ctx, &out->frames[frame].stack_depth);
    digest_frame(ctx, &out->frames[frame]);
    out->command_total += out->frames[frame].command_count;
    if (rt.scroll_container) {
      out->max_scroll_y = mu_max(out->max_scroll_y,
        rt.scroll_container->scroll.y);
    }
  }

  out->alloc_calls = g_alloc_calls - calls_before;
  out->free_calls = g_free_calls - frees_before;
  out->live_bytes = g_live_bytes - live_before;
  free(ctx);
}

static int count_reachable(const Case *c, int idx) {
  const Node *n = &c->nodes[idx];
  int count = 1;
  int i;
  for (i = 0; i < n->nkids; i++) {
    count += count_reachable(c, n->kids[i]);
  }
  return count;
}

static EvalResult evaluate_case(const Case *c) {
  EvalResult result;
  Case run_a = *c;
  Case run_b = *c;
  RunTrace a;
  RunTrace b;
  long live_before = g_live_bytes;
  long calls_before = g_alloc_calls;
  int frame;
  memset(&result, 0, sizeof(result));
  result.tree_nodes = count_reachable(c, c->root);

  run_trace(&run_a, &a);
  run_trace(&run_b, &b);
  result.alloc_calls = g_alloc_calls - calls_before;
  result.alloc_live = g_live_bytes - live_before;
  result.command_total = (int) a.command_total;
  result.max_scroll_y = a.max_scroll_y;

  for (frame = 0; frame < FRAMES; frame++) {
    if (a.frames[frame].stack_failure || b.frames[frame].stack_failure) {
      result.kind = F_STACK;
      result.frame = frame;
      result.stack_index = a.frames[frame].stack_failure
        ? a.frames[frame].stack_depth : b.frames[frame].stack_depth;
      snprintf(result.detail, sizeof(result.detail),
        "stack leak in frame %d, stack=%d", frame, result.stack_index);
      return result;
    }
  }

  if (result.alloc_live != 0) {
    result.kind = F_LEAK;
    snprintf(result.detail, sizeof(result.detail),
      "%ld bytes still allocated after layout", result.alloc_live);
    return result;
  }

  for (frame = 0; frame < FRAMES; frame++) {
    if (a.frames[frame].command_count != b.frames[frame].command_count ||
        a.frames[frame].command_hash != b.frames[frame].command_hash ||
        a.frames[frame].state_hash != b.frames[frame].state_hash) {
      result.kind = F_NONDET;
      result.frame = frame;
      result.count_a = a.frames[frame].command_count;
      result.count_b = b.frames[frame].command_count;
      result.hash_a = a.frames[frame].command_hash ^
        a.frames[frame].state_hash;
      result.hash_b = b.frames[frame].command_hash ^
        b.frames[frame].state_hash;
      snprintf(result.detail, sizeof(result.detail),
        "frame %d: run A commands=%d hash=%016llx, "
        "run B commands=%d hash=%016llx",
        frame, result.count_a, (unsigned long long) result.hash_a,
        result.count_b, (unsigned long long) result.hash_b);
      return result;
    }
  }

  result.kind = F_PASS;
  return result;
}

static void redirect_child_stderr(void) {
  int fd = open("/dev/null", O_WRONLY);
  if (fd >= 0) {
    dup2(fd, STDERR_FILENO);
    close(fd);
  }
}

static void run_isolated(const Case *c, EvalResult *result) {
  int fds[2];
  pid_t pid;
  ssize_t got = 0;
  int status = 0;
  int saved_errno;
  memset(result, 0, sizeof(*result));

  if (pipe(fds) != 0) {
    result->kind = F_INTERNAL;
    snprintf(result->detail, sizeof(result->detail),
      "pipe failed: %s", strerror(errno));
    return;
  }
  pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    result->kind = F_INTERNAL;
    snprintf(result->detail, sizeof(result->detail),
      "fork failed: %s", strerror(errno));
    return;
  }
  if (pid == 0) {
    EvalResult child_result;
    close(fds[0]);
    alarm(EVAL_TIMEOUT_SECONDS);
    if (!getenv("PROP_DEBUG")) { redirect_child_stderr(); }
    child_result = evaluate_case(c);
    if (write(fds[1], &child_result, sizeof(child_result)) !=
        (ssize_t) sizeof(child_result)) {
      _exit(2);
    }
    close(fds[1]);
    _exit(0);
  }

  close(fds[1]);
  while (got < (ssize_t) sizeof(*result)) {
    ssize_t n = read(fds[0], (char *) result + got,
      sizeof(*result) - (size_t) got);
    if (n > 0) {
      got += n;
      continue;
    }
    if (n == 0) { break; }
    if (errno == EINTR) { continue; }
    break;
  }
  close(fds[0]);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  saved_errno = errno;
  (void) saved_errno;

  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    memset(result, 0, sizeof(*result));
    result->kind = (sig == SIGALRM) ? F_TIMEOUT : F_CRASH;
    result->signal = sig;
    if (sig == SIGALRM) {
      snprintf(result->detail, sizeof(result->detail),
        "timed out after %d seconds", EVAL_TIMEOUT_SECONDS);
    } else {
      snprintf(result->detail, sizeof(result->detail),
        "terminated by signal %d (%s)", sig, strsignal(sig));
    }
    return;
  }
  if (got != (ssize_t) sizeof(*result)) {
    memset(result, 0, sizeof(*result));
    result->kind = F_CRASH;
    snprintf(result->detail, sizeof(result->detail),
      "child exited without a complete result (got %ld/%lu bytes, status=%d)",
      (long) got, (unsigned long) sizeof(*result), status);
    return;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    memset(result, 0, sizeof(*result));
    result->kind = F_CRASH;
    snprintf(result->detail, sizeof(result->detail),
      "child exited with status %d", status);
  }
}

static int same_failure(const EvalResult *a, const EvalResult *b) {
  if (a->kind != b->kind) { return 0; }
  if (a->kind == F_CRASH || a->kind == F_TIMEOUT) {
    return a->signal == b->signal;
  }
  if (a->kind == F_STACK) {
    return a->stack_index == b->stack_index;
  }
  return 1;
}

static void remove_child_reference(Case *c, int idx) {
  Node *n = &c->nodes[idx];
  Node *parent;
  int i;
  int j;
  if (n->parent < 0) { return; }
  parent = &c->nodes[n->parent];
  for (i = 0; i < parent->nkids; i++) {
    if (parent->kids[i] == idx) {
      for (j = i; j + 1 < parent->nkids; j++) {
        parent->kids[j] = parent->kids[j + 1];
      }
      parent->nkids--;
      return;
    }
  }
}

static int collect_reachable(const Case *c, int idx, int *out, int cap) {
  const Node *n = &c->nodes[idx];
  int count = 0;
  int i;
  if (count >= cap) { return count; }
  out[count++] = idx;
  for (i = 0; i < n->nkids && count < cap; i++) {
    count += collect_reachable(c, n->kids[i], out + count, cap - count);
  }
  return count;
}

static void count_node_kinds(const Case *c, int idx, int *counts) {
  const Node *n = &c->nodes[idx];
  int i;
  if (n->kind >= 0 && n->kind < N_NONE) { counts[n->kind]++; }
  for (i = 0; i < n->nkids; i++) {
    count_node_kinds(c, n->kids[i], counts);
  }
}

static int splice_node(Case *c, int idx) {
  Node *n = &c->nodes[idx];
  Node *parent;
  int kids[MAX_KIDS];
  int kid_count = n->nkids;
  int pos = -1;
  int i;
  int j;
  if (n->parent < 0 || !node_is_feature(n->kind) || kid_count == 0) {
    return 0;
  }
  parent = &c->nodes[n->parent];
  if (parent->nkids - 1 + kid_count > MAX_KIDS) { return 0; }
  for (i = 0; i < kid_count; i++) { kids[i] = n->kids[i]; }
  for (i = 0; i < parent->nkids; i++) {
    if (parent->kids[i] == idx) {
      pos = i;
      break;
    }
  }
  if (pos < 0) { return 0; }
  for (j = parent->nkids - 1; j > pos; j--) {
    parent->kids[j + kid_count - 1] = parent->kids[j];
  }
  for (i = 0; i < kid_count; i++) {
    parent->kids[pos + i] = kids[i];
    c->nodes[kids[i]].parent = n->parent;
  }
  parent->nkids += kid_count - 1;
  return 1;
}

static int try_failure(const Case *c, const EvalResult *wanted,
  EvalResult *result)
{
  run_isolated(c, result);
  return same_failure(wanted, result);
}

static int simplify_int(Case *c, int idx, int field, int value) {
  Node *n = &c->nodes[idx];
  if (field == 0) { n->width = value; }
  else if (field == 1) { n->height = value; }
  else if (field == 2) { n->opt = value; }
  else if (field == 3) { n->phase = value; }
  else if (field == 4) { n->clip_dx = value; }
  else if (field == 5) { n->clip_dy = value; }
  else if (field == 6) { n->clip_w = value; }
  else if (field == 7) { n->clip_h = value; }
  else if (field == 8) { n->checked = value; }
  else { return 0; }
  return 1;
}

static Case minimize_case(const Case *initial, EvalResult *failure) {
  Case current = *initial;
  EvalResult current_failure = *failure;
  int iteration;

  for (iteration = 0; iteration < SHRINK_LIMIT; iteration++) {
    int reachable[MAX_NODES];
    int count = collect_reachable(&current, current.root, reachable,
      MAX_NODES);
    int changed = 0;
    int i;
    int j;

    for (i = 1; i < count; i++) {
      int idx = reachable[i];
      Case candidate = current;
      EvalResult trial;
      remove_child_reference(&candidate, idx);
      if (try_failure(&candidate, &current_failure, &trial)) {
        current = candidate;
        current_failure = trial;
        changed = 1;
        break;
      }
    }
    if (changed) { continue; }

    for (i = 1; i < count; i++) {
      int idx = reachable[i];
      Case candidate = current;
      EvalResult trial;
      if (!splice_node(&candidate, idx)) { continue; }
      if (try_failure(&candidate, &current_failure, &trial)) {
        current = candidate;
        current_failure = trial;
        changed = 1;
        break;
      }
    }
    if (changed) { continue; }

    count = collect_reachable(&current, current.root, reachable, MAX_NODES);
    for (i = 0; i < count && !changed; i++) {
      int idx = reachable[i];
      int fields[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
      for (j = 0; j < ARRAY_LEN(fields); j++) {
        static const int zero_values[][4] = {
          { 0, -1, 1, 8 }, { 0, 1, 8, 16 }, { 0, 0, 0, 0 },
          { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
          { 1, 8, 16, 32 }, { 1, 8, 16, 32 }, { 0, 0, 0, 0 }
        };
        int k;
        for (k = 0; k < 4; k++) {
          Case candidate = current;
          EvalResult trial;
          int value = zero_values[j][k];
          Node *original = &current.nodes[idx];
          if (fields[j] == 0 && original->width == value) { continue; }
          if (fields[j] == 1 && original->height == value) { continue; }
          if (fields[j] == 2 && original->opt == value) { continue; }
          if (fields[j] == 3 && original->phase == value) { continue; }
          if (fields[j] == 4 && original->clip_dx == value) { continue; }
          if (fields[j] == 5 && original->clip_dy == value) { continue; }
          if (fields[j] == 6 && original->clip_w == value) { continue; }
          if (fields[j] == 7 && original->clip_h == value) { continue; }
          if (fields[j] == 8 && original->checked == value) { continue; }
          simplify_int(&candidate, idx, fields[j], value);
          if (try_failure(&candidate, &current_failure, &trial)) {
            current = candidate;
            current_failure = trial;
            changed = 1;
            break;
          }
        }
        if (changed) { break; }
      }
    }
    if (!changed) { break; }
  }

  *failure = current_failure;
  return current;
}

static void print_opt(FILE *out, int opt) {
  int first = 1;
  if (!opt) {
    fputs("0", out);
    return;
  }
#define PRINT_OPT(bit, name) do {                    \
    if (opt & (bit)) {                               \
      fprintf(out, "%s%s", first ? "" : "|", name);  \
      first = 0;                                     \
    }                                                \
  } while (0)
  PRINT_OPT(MU_OPT_NOSCROLL, "NOSCROLL");
  PRINT_OPT(MU_OPT_NORESIZE, "NORESIZE");
  PRINT_OPT(MU_OPT_NOCLOSE, "NOCLOSE");
  PRINT_OPT(MU_OPT_NOTITLE, "NOTITLE");
  PRINT_OPT(MU_OPT_NOFRAME, "NOFRAME");
  PRINT_OPT(MU_OPT_NOINTERACT, "NOINTERACT");
#undef PRINT_OPT
}

static void print_tree_node(FILE *out, const Case *c, int idx, int depth) {
  const Node *n = &c->nodes[idx];
  int i;
  for (i = 0; i < depth; i++) { fputs("  ", out); }
  switch (n->kind) {
  case N_WINDOW:
    fprintf(out, "window \"root\" rect=(%d,%d,%d,%d) opt=",
      c->rect_x, c->rect_y, c->rect_w, c->rect_h);
    print_opt(out, n->opt);
    break;
  case N_PANEL:
    fprintf(out, "panel \"%s\" cell=(%d,%d) opt=",
      n->label, n->width, n->height);
    print_opt(out, n->opt);
    break;
  case N_SCROLL:
    fprintf(out, "scroll \"%s\" cell=(%d,%d)", n->label, n->width, n->height);
    break;
  case N_CLIP:
    fprintf(out, "clip cell=(%d,%d) rect=(%d,%d,%d,%d)",
      n->width, n->height, n->clip_dx, n->clip_dy, n->clip_w, n->clip_h);
    break;
  case N_HIDDEN:
    fprintf(out, "hidden cell=(%d,%d) phase=%d",
      n->width, n->height, n->phase);
    break;
  case N_LABEL:
    fprintf(out, "label \"%s\" cell=(%d,%d)", n->label, n->width, n->height);
    break;
  case N_BUTTON:
    fprintf(out, "button \"%s\" cell=(%d,%d) opt=",
      n->label, n->width, n->height);
    print_opt(out, n->opt);
    break;
  case N_CHECKBOX:
    fprintf(out, "checkbox \"%s\" cell=(%d,%d) checked=%d",
      n->label, n->width, n->height, n->checked);
    break;
  case N_SLIDER:
    fprintf(out, "slider cell=(%d,%d) value=%.3g",
      n->width, n->height, (double) n->value);
    break;
  case N_TEXTBOX:
    fprintf(out, "textbox cell=(%d,%d) text=\"%s\"",
      n->width, n->height, n->text);
    break;
  default:
    fprintf(out, "none");
    break;
  }

  if (n->nkids) {
    fputs(" {\n", out);
    for (i = 0; i < n->nkids; i++) {
      print_tree_node(out, c, n->kids[i], depth + 1);
    }
    for (i = 0; i < depth; i++) { fputs("  ", out); }
    fputs("}\n", out);
  } else {
    fputs("\n", out);
  }
}

static void print_failure(FILE *out, int case_no, const Case *c,
  const EvalResult *e)
{
  fprintf(out,
    "\ncounterexample: case=%d seed=0x%016llx nodes=%d failure=%s\n",
    case_no, (unsigned long long) c->seed, count_reachable(c, c->root),
    failure_names[e->kind]);
  if (e->detail[0]) { fprintf(out, "reason: %s\n", e->detail); }
  print_tree_node(out, c, c->root, 0);
}

typedef struct FailureBucket {
  EvalResult result;
  int case_no;
  Case minimized;
  int count;
} FailureBucket;

static int failure_bucket_for(FailureBucket *buckets, int *bucket_count,
  const EvalResult *e)
{
  int i;
  for (i = 0; i < *bucket_count; i++) {
    if (same_failure(&buckets[i].result, e)) { return i; }
  }
  return -1;
}

static double elapsed_seconds(struct timespec start, struct timespec end) {
  return (double) (end.tv_sec - start.tv_sec) +
    (double) (end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
  int case_count = 200;
  uint64_t base_seed = UINT64_C(0x6d6963726f7569);
  int shrink = 1;
  int verbose = 0;
  FailureBucket buckets[MAX_UNIQUE_FAILURES];
  int bucket_count = 0;
  int passed = 0;
  int failed = 0;
  long total_commands = 0;
  int scrolled_cases = 0;
  int generated[N_NONE];
  int i;
  struct timespec start;
  struct timespec end;

  (void) test_calloc;
  (void) test_realloc;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--cases") && i + 1 < argc) {
      case_count = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      base_seed = strtoull(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--no-shrink")) {
      shrink = 0;
    } else if (!strcmp(argv[i], "--verbose")) {
      verbose = 1;
    } else {
      fprintf(stderr,
        "usage: %s [--cases N] [--seed N] [--no-shrink] [--verbose]\n",
        argv[0]);
      return 2;
    }
  }
  if (case_count <= 0) {
    fprintf(stderr, "case count must be positive\n");
    return 2;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);
  memset(buckets, 0, sizeof(buckets));
  memset(generated, 0, sizeof(generated));
  printf("microui property test: cases=%d seed=0x%016llx frames=%d runs=2\n",
    case_count, (unsigned long long) base_seed, FRAMES);

  for (i = 0; i < case_count; i++) {
    Case c;
    EvalResult result;
    int bucket;
    gen_case(&c, base_seed + (uint64_t) i * UINT64_C(0x9e3779b97f4a7c15));
    count_node_kinds(&c, c.root, generated);
    run_isolated(&c, &result);
    total_commands += result.command_total;
    if (result.max_scroll_y > 0) { scrolled_cases++; }
    if (result.kind == F_PASS) {
      passed++;
      if (verbose) {
        printf("case %03d: pass (%d nodes)\n", i, count_reachable(&c, c.root));
      }
      continue;
    }

    failed++;
    bucket = failure_bucket_for(buckets, &bucket_count, &result);
    printf("case %03d: FAIL %s", i, failure_names[result.kind]);
    if (result.detail[0]) { printf(": %s", result.detail); }
    putchar('\n');
    if (bucket < 0) {
      if (bucket_count < MAX_UNIQUE_FAILURES) {
        bucket = bucket_count++;
        buckets[bucket].result = result;
        buckets[bucket].case_no = i;
        buckets[bucket].minimized = c;
        buckets[bucket].count = 1;
        if (shrink) {
          printf("case %03d: shrinking (%d nodes)...\n", i,
            count_reachable(&c, c.root));
          buckets[bucket].minimized = minimize_case(&c, &result);
          buckets[bucket].result = result;
          printf("case %03d: minimized to %d nodes\n", i,
            count_reachable(&buckets[bucket].minimized,
              buckets[bucket].minimized.root));
        }
      }
    } else {
      buckets[bucket].count++;
    }
    if ((i + 1) % 25 == 0 || i + 1 == case_count) {
      printf("progress: %d/%d\n", i + 1, case_count);
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  printf("\nsummary\n");
  printf("  cases:   %d\n", case_count);
  printf("  passed:  %d\n", passed);
  printf("  failed:  %d\n", failed);
  printf("  layout:  %d frames, %ld commands, %d cases with non-zero scroll\n",
    case_count * FRAMES, total_commands, scrolled_cases);
  printf("  elapsed: %.3f s\n", elapsed_seconds(start, end));
  printf("  features: panel=%d clip=%d scroll=%d hidden=%d\n",
    generated[N_PANEL], generated[N_CLIP], generated[N_SCROLL],
    generated[N_HIDDEN]);
  printf("  controls: label=%d button=%d checkbox=%d slider=%d textbox=%d\n",
    generated[N_LABEL], generated[N_BUTTON], generated[N_CHECKBOX],
    generated[N_SLIDER], generated[N_TEXTBOX]);
  printf("  failures by kind:");
  if (!bucket_count) {
    printf(" none");
  } else {
    for (i = 0; i < bucket_count; i++) {
      printf(" %s=%d", failure_names[buckets[i].result.kind],
        buckets[i].count);
    }
  }
  putchar('\n');

  for (i = 0; i < bucket_count; i++) {
    print_failure(stdout, buckets[i].case_no, &buckets[i].minimized,
      &buckets[i].result);
  }

  return failed ? 1 : 0;
}
