/* stack_bounds.c — reproduction sequences and boundary scan for clip/id
** stack imbalance handling in microui.
**
** Each scenario below used to abort() the process (or read out of bounds);
** after the fix it must report a clear error, keep every stack index within
** [0, capacity], recover to a clean state and never crash.
**
** Build & run:
**   cc -std=c89 -Wall -Wextra -fsanitize=address,undefined \
**      -Isrc test/stack_bounds.c src/microui.c -o /tmp/stack_bounds
**   /tmp/stack_bounds
*/
#include <stdio.h>
#include <string.h>
#include "microui.h"

static int checks = 0;
static int failures = 0;

static int text_width(mu_Font font, const char *text, int len) {
  (void) font; (void) text;
  return len * 8;
}

static int text_height(mu_Font font) {
  (void) font;
  return 16;
}

#define CHECK(cond, label) do {                                   \
    checks++;                                                     \
    if (!(cond)) {                                                \
      failures++;                                                 \
      printf("  FAIL: %s\n", label);                              \
    }                                                             \
  } while (0)

/* invariant: a stack index must always be within [0, capacity] */
#define CHECK_BOUNDS(stk, cap, label) do {                        \
    checks++;                                                     \
    if ((stk).idx < 0 || (stk).idx > (cap)) {                     \
      failures++;                                                 \
      printf("  FAIL: %s idx=%d out of [0,%d]\n",                 \
        label, (stk).idx, (cap));                                 \
    }                                                             \
  } while (0)

static void init(mu_Context *ctx) {
  mu_init(ctx);
  ctx->text_width = text_width;
  ctx->text_height = text_height;
}

/* mimic begin_root_container(): seed the base clip rect directly, so the
** scan can fill the stack to exactly its capacity like real frames do */
static void seed_clip(mu_Context *ctx) {
  ctx->clip_stack.items[0] = mu_rect(0, 0, 0x1000000, 0x1000000);
  ctx->clip_stack.idx = 1;
}

/* run one ordinary, fully balanced frame; used to prove that the context
** is back to a correct working state after an error was recovered */
static int run_clean_frame(mu_Context *ctx) {
  mu_Command *cmd = NULL;
  int commands = 0;
  mu_begin(ctx);
  if (mu_begin_window(ctx, "Clean", mu_rect(10, 10, 200, 150))) {
    mu_button(ctx, "ok");
    mu_end_window(ctx);
  }
  mu_end(ctx);
  while (mu_next_command(ctx, &cmd)) { commands++; }
  return commands;
}

/* R1: pop on an empty clip stack (underflow) */
static void repro_underflow(mu_Context *ctx) {
  printf("R1 pop on empty clip stack (underflow)\n");
  mu_begin(ctx);
  mu_pop_clip_rect(ctx);
  CHECK(ctx->error == MU_ERROR_STACK_UNDERFLOW, "R1 error code");
  CHECK(ctx->clip_stack.idx == 0, "R1 idx stays 0");
  mu_end(ctx);
  CHECK_BOUNDS(ctx->clip_stack, MU_CLIPSTACK_SIZE, "R1 clip_stack");
  mu_clear_error(ctx);
  CHECK(ctx->error == MU_ERROR_NONE, "R1 cleared");
  CHECK(run_clean_frame(ctx) > 0 && ctx->error == MU_ERROR_NONE,
    "R1 recovery frame");
}

/* R2: push past MU_CLIPSTACK_SIZE (overflow) */
static void repro_overflow(mu_Context *ctx) {
  int i;
  printf("R2 push past clip stack capacity (overflow)\n");
  mu_begin(ctx);
  seed_clip(ctx);
  for (i = 1; i < MU_CLIPSTACK_SIZE; i++) {
    mu_push_clip_rect(ctx, mu_rect(0, 0, 100, 100));
  }
  CHECK(ctx->error == MU_ERROR_NONE, "R2 no error at capacity");
  mu_push_clip_rect(ctx, mu_rect(0, 0, 100, 100));
  CHECK(ctx->error == MU_ERROR_STACK_OVERFLOW, "R2 error code");
  CHECK(ctx->clip_stack.idx == MU_CLIPSTACK_SIZE, "R2 idx stays at cap");
  for (i = 0; i < MU_CLIPSTACK_SIZE; i++) { mu_pop_clip_rect(ctx); }
  mu_end(ctx);
  CHECK(ctx->error == MU_ERROR_STACK_OVERFLOW, "R2 sticky first error");
  CHECK(ctx->clip_stack.idx == 0, "R2 drained to 0");
  mu_clear_error(ctx);
  CHECK(run_clean_frame(ctx) > 0 && ctx->error == MU_ERROR_NONE,
    "R2 recovery frame");
}

/* R3: mu_begin_window without mu_end_window (unbalanced frame) */
static void repro_unbalanced(mu_Context *ctx) {
  printf("R3 begin_window without end_window (unbalanced)\n");
  mu_begin(ctx);
  if (mu_begin_window(ctx, "Unbalanced", mu_rect(10, 10, 200, 150))) {
    mu_button(ctx, "leak");
    /* missing mu_end_window() on purpose */
  }
  mu_end(ctx);
  CHECK(ctx->error == MU_ERROR_STACK_UNBALANCED, "R3 error code");
  CHECK(ctx->container_stack.idx == 0, "R3 container stack reset");
  CHECK(ctx->clip_stack.idx == 0, "R3 clip stack reset");
  CHECK(ctx->id_stack.idx == 0, "R3 id stack reset");
  CHECK(ctx->layout_stack.idx == 0, "R3 layout stack reset");
  mu_clear_error(ctx);
  CHECK(run_clean_frame(ctx) > 0 && ctx->error == MU_ERROR_NONE,
    "R3 recovery frame");
}

/* R4: query the clip rect with an empty clip stack */
static void repro_get_clip_empty(mu_Context *ctx) {
  mu_Rect r;
  printf("R4 get_clip_rect on empty stack\n");
  mu_begin(ctx);
  r = mu_get_clip_rect(ctx);
  CHECK(ctx->error == MU_ERROR_STACK_UNDERFLOW, "R4 error code");
  CHECK(r.x == 0 && r.y == 0 && r.w == 0x1000000 && r.h == 0x1000000,
    "R4 falls back to unclipped rect");
  mu_end(ctx);
  mu_clear_error(ctx);
  CHECK(run_clean_frame(ctx) > 0 && ctx->error == MU_ERROR_NONE,
    "R4 recovery frame");
}

/* R5: mu_end_window without a matching begin (extra pop) */
static void repro_extra_end(mu_Context *ctx) {
  printf("R5 end_window without begin_window (extra pop)\n");
  mu_begin(ctx);
  mu_end_window(ctx);
  CHECK(ctx->error != MU_ERROR_NONE, "R5 error reported");
  CHECK_BOUNDS(ctx->container_stack, MU_CONTAINERSTACK_SIZE, "R5 container");
  CHECK_BOUNDS(ctx->clip_stack, MU_CLIPSTACK_SIZE, "R5 clip");
  CHECK_BOUNDS(ctx->id_stack, MU_IDSTACK_SIZE, "R5 id");
  CHECK_BOUNDS(ctx->layout_stack, MU_LAYOUTSTACK_SIZE, "R5 layout");
  mu_end(ctx);
  mu_clear_error(ctx);
  CHECK(run_clean_frame(ctx) > 0 && ctx->error == MU_ERROR_NONE,
    "R5 recovery frame");
}

/* boundary scan: walk each stack to its capacity edge and one step past
** it in both directions, checking the index invariant after every step */
static void boundary_scan(mu_Context *ctx) {
  int i;
  int base_errors;
  printf("boundary scan: clip stack (cap %d)\n", MU_CLIPSTACK_SIZE);
  mu_begin(ctx);
  seed_clip(ctx);
  base_errors = ctx->error_count;
  for (i = 1; i < MU_CLIPSTACK_SIZE; i++) {
    mu_push_clip_rect(ctx, mu_rect(i, i, 100, 100));
    CHECK_BOUNDS(ctx->clip_stack, MU_CLIPSTACK_SIZE, "scan clip push");
  }
  CHECK(ctx->error_count == base_errors, "scan clip: cap pushes clean");
  mu_push_clip_rect(ctx, mu_rect(0, 0, 1, 1));
  CHECK(ctx->error == MU_ERROR_STACK_OVERFLOW, "scan clip: cap+1 overflow");
  CHECK(ctx->clip_stack.idx == MU_CLIPSTACK_SIZE, "scan clip: idx==cap");
  for (i = 0; i < MU_CLIPSTACK_SIZE; i++) {
    mu_pop_clip_rect(ctx);
    CHECK_BOUNDS(ctx->clip_stack, MU_CLIPSTACK_SIZE, "scan clip pop");
  }
  mu_pop_clip_rect(ctx);
  CHECK(ctx->clip_stack.idx == 0, "scan clip: idx==0 after extra pop");
  mu_end(ctx);
  mu_clear_error(ctx);

  printf("boundary scan: id stack (cap %d)\n", MU_IDSTACK_SIZE);
  mu_begin(ctx);
  base_errors = ctx->error_count;
  for (i = 0; i < MU_IDSTACK_SIZE; i++) {
    mu_push_id(ctx, "x", 1);
    CHECK_BOUNDS(ctx->id_stack, MU_IDSTACK_SIZE, "scan id push");
  }
  CHECK(ctx->error_count == base_errors, "scan id: cap pushes clean");
  mu_push_id(ctx, "x", 1);
  CHECK(ctx->error == MU_ERROR_STACK_OVERFLOW, "scan id: cap+1 overflow");
  CHECK(ctx->id_stack.idx == MU_IDSTACK_SIZE, "scan id: idx==cap");
  for (i = 0; i < MU_IDSTACK_SIZE; i++) {
    mu_pop_id(ctx);
    CHECK_BOUNDS(ctx->id_stack, MU_IDSTACK_SIZE, "scan id pop");
  }
  mu_pop_id(ctx);
  CHECK(ctx->id_stack.idx == 0, "scan id: idx==0 after extra pop");
  mu_end(ctx);
  mu_clear_error(ctx);

  CHECK(run_clean_frame(ctx) > 0 && ctx->error == MU_ERROR_NONE,
    "scan recovery frame");
}

int main(void) {
  static mu_Context ctx;
  init(&ctx);
  repro_underflow(&ctx);
  repro_overflow(&ctx);
  repro_unbalanced(&ctx);
  repro_get_clip_empty(&ctx);
  repro_extra_end(&ctx);
  boundary_scan(&ctx);
  printf("----\nboundary scan data: %d checks, %d failures, "
    "%d errors reported (all recovered)\n",
    checks, failures, ctx.error_count);
  if (failures) {
    printf("RESULT: FAIL\n");
    return 1;
  }
  printf("RESULT: PASS (zero crashes)\n");
  return 0;
}
