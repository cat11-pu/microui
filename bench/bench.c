/*
** Benchmark + pixel-comparison harness for microui command generation.
**
** Builds a UI scene of 4 windows x `rows` control rows (most rows are
** scrolled out of view), measures the per-frame cost of rebuilding the
** draw-command list (mu_begin..mu_end) and of consuming it, rasterizes
** every frame with a deterministic software renderer (src-over blending,
** matching demo/renderer.c's GL blend func) and hashes each frame so two
** builds can be compared pixel-for-pixel.
**
** Usage: bench_<tag> <rows-per-window> [frames]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "microui.h"
#include "atlas.inl"

#ifndef TAG
#define TAG "dev"
#endif

enum { FBW = 1280, FBH = 720 };
enum { NW = 4, MAXR = 400 };

static uint32_t fb[FBW * FBH];
static mu_Rect fb_scissor;

/*============================================================================
** software renderer (deterministic, src-over alpha like GL_SRC_ALPHA /
** GL_ONE_MINUS_SRC_ALPHA in demo/renderer.c)
**============================================================================*/

static void blend_pixel(int x, int y, unsigned r, unsigned g, unsigned b,
  unsigned a)
{
  uint32_t *p;
  unsigned dr, dg, db;
  if ((unsigned) x >= FBW || (unsigned) y >= FBH) { return; }
  p = &fb[y * FBW + x];
  dr = *p & 0xff;
  dg = (*p >> 8) & 0xff;
  db = (*p >> 16) & 0xff;
  *p = 0xff000000u
    | (((b * a + db * (255 - a) + 127) / 255) << 16)
    | (((g * a + dg * (255 - a) + 127) / 255) << 8)
    | ((r * a + dr * (255 - a) + 127) / 255);
}


static void draw_rect(mu_Rect r, mu_Color c) {
  int x0 = mu_max(r.x, 0), y0 = mu_max(r.y, 0);
  int x1 = mu_min(r.x + r.w, FBW), y1 = mu_min(r.y + r.h, FBH);
  int x, y;
  if (c.a == 255) {
    uint32_t v = 0xff000000u | ((uint32_t) c.b << 16) | (c.g << 8) | c.r;
    for (y = y0; y < y1; y++) {
      for (x = x0; x < x1; x++) { fb[y * FBW + x] = v; }
    }
  } else {
    for (y = y0; y < y1; y++) {
      for (x = x0; x < x1; x++) { blend_pixel(x, y, c.r, c.g, c.b, c.a); }
    }
  }
}


static void draw_sprite(mu_Rect dst, mu_Rect src, mu_Color c) {
  int x, y;
  int x0 = mu_max(mu_max(dst.x, fb_scissor.x), 0);
  int y0 = mu_max(mu_max(dst.y, fb_scissor.y), 0);
  int x1 = mu_min(mu_min(dst.x + dst.w, fb_scissor.x + fb_scissor.w), FBW);
  int y1 = mu_min(mu_min(dst.y + dst.h, fb_scissor.y + fb_scissor.h), FBH);
  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      unsigned t = atlas_texture[
        (src.y + (y - dst.y)) * ATLAS_WIDTH + (src.x + (x - dst.x))];
      unsigned a = (t * c.a) / 255;
      if (a) { blend_pixel(x, y, c.r, c.g, c.b, a); }
    }
  }
}


static void draw_text(const char *text, mu_Vec2 pos, mu_Color c) {
  mu_Rect dst = { pos.x, pos.y, 0, 0 };
  const char *p;
  for (p = text; *p; p++) {
    int chr;
    mu_Rect src;
    if ((*p & 0xc0) == 0x80) { continue; }
    chr = mu_min((unsigned char) *p, 127);
    src = atlas[ATLAS_FONT + chr];
    dst.w = src.w;
    dst.h = src.h;
    draw_sprite(dst, src, c);
    dst.x += dst.w;
  }
}


static void draw_icon(int id, mu_Rect rect, mu_Color c) {
  mu_Rect src = atlas[id];
  int x = rect.x + (rect.w - src.w) / 2;
  int y = rect.y + (rect.h - src.h) / 2;
  draw_sprite(mu_rect(x, y, src.w, src.h), src, c);
}


static int text_width_cb(mu_Font font, const char *text, int len) {
  int res = 0;
  const char *p;
  (void) font;
  if (len < 0) { len = (int) strlen(text); }
  for (p = text; *p && len--; p++) {
    if ((*p & 0xc0) == 0x80) { continue; }
    res += atlas[ATLAS_FONT + mu_min((unsigned char) *p, 127)].w;
  }
  return res;
}


static int text_height_cb(mu_Font font) {
  (void) font;
  return 18;
}

/*============================================================================
** command-list consumer: rasterizes and counts draw commands / material
** batches (a batch = run of consecutive draw commands sharing type+color)
**============================================================================*/

static unsigned long stat_cmds;
static unsigned long stat_batches;

static uint64_t color_key(mu_Color c) {
  return ((uint64_t) c.r << 24) | (c.g << 16) | (c.b << 8) | c.a;
}


static void render(mu_Context *ctx) {
  mu_Command *cmd = NULL;
  uint64_t mat = 0, last_mat = 0;
  int have_mat = 0;
  fb_scissor = mu_rect(0, 0, 0x1000000, 0x1000000);
  while (mu_next_command(ctx, &cmd)) {
    stat_cmds++;
    switch (cmd->type) {
      case MU_COMMAND_CLIP:
        fb_scissor = cmd->clip.rect;
        have_mat = 0; /* renderer would flush on clip change */
        continue;
      case MU_COMMAND_RECT:
        mat = (1ull << 40) | color_key(cmd->rect.color);
        draw_rect(cmd->rect.rect, cmd->rect.color);
        break;
      case MU_COMMAND_TEXT:
        mat = (2ull << 40) | color_key(cmd->text.color);
        draw_text(cmd->text.str, cmd->text.pos, cmd->text.color);
        break;
      case MU_COMMAND_ICON:
        mat = (3ull << 40) | ((uint64_t) cmd->icon.id << 32);
        draw_icon(cmd->icon.id, cmd->icon.rect, cmd->icon.color);
        break;
      default:
        continue;
    }
    if (!have_mat || mat != last_mat) { stat_batches++; }
    last_mat = mat;
    have_mat = 1;
  }
}

/*============================================================================
** scene
**============================================================================*/

static char btn_labels[NW][MAXR][16];
static char row_texts[NW][MAXR][24];
static mu_Real slider_vals[NW][MAXR];
static int check_states[NW][MAXR];
static char tbuf[NW][64];
static const char *win_titles[NW] = { "Inventory", "Character", "Map", "Chat" };

static void init_scene(void) {
  int w, i;
  for (w = 0; w < NW; w++) {
    for (i = 0; i < MAXR; i++) {
      sprintf(btn_labels[w][i], "btn_%d_%03d", w, i);
      sprintf(row_texts[w][i], "row %d item %03d", w, i);
      slider_vals[w][i] = (mu_Real) ((i * 37 + w * 11) % 100);
      check_states[w][i] = (i + w) & 1;
    }
    sprintf(tbuf[w], "textbox %d", w);
  }
}


static int build_scene(mu_Context *ctx, int rows) {
  int widgets = 0;
  int w, i, t;
  for (w = 0; w < NW; w++) {
    if (mu_begin_window(ctx, win_titles[w], mu_rect(20 + w * 320, 30, 300, 620))) {
      mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
      mu_textbox(ctx, tbuf[w], sizeof(tbuf[w]));
      widgets++;
      mu_header(ctx, "Section");
      widgets++;
      for (t = 0; t < 3; t++) {
        if (mu_begin_treenode(ctx, t == 0 ? "Stats" : t == 1 ? "Skills" : "Buffs")) {
          mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
          mu_label(ctx, "nested label a");
          mu_label(ctx, "nested label b");
          widgets += 2;
          mu_end_treenode(ctx);
        }
        widgets++;
      }
      mu_layout_row(ctx, 1, (int[]) { -1 }, 120);
      mu_begin_panel(ctx, "Preview");
      mu_layout_row(ctx, 2, (int[]) { 80, -1 }, 0);
      mu_button(ctx, "Apply");
      mu_button(ctx, "Cancel");
      mu_layout_row(ctx, 1, (int[]) { -1 }, 0);
      mu_label(ctx, "preview pane");
      widgets += 3;
      mu_end_panel(ctx);
      for (i = 0; i < rows; i++) {
        mu_layout_row(ctx, 4, (int[]) { 64, 76, 60, -1 }, 0);
        mu_button(ctx, btn_labels[w][i]);
        mu_slider(ctx, &slider_vals[w][i], 0, 100);
        mu_checkbox(ctx, "chk", &check_states[w][i]);
        mu_label(ctx, row_texts[w][i]);
        widgets += 4;
      }
      mu_end_window(ctx);
    }
  }
  return widgets;
}

/*============================================================================
** main
**============================================================================*/

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}


static uint64_t hash_fb(void) {
  uint64_t h = 1469598103934665603ull;
  const unsigned char *p = (const unsigned char *) fb;
  size_t n = sizeof(fb);
  while (n--) { h = (h ^ *p++) * 1099511628211ull; }
  return h;
}


int main(int argc, char **argv) {
  static mu_Context ctx;
  int rows = argc > 1 ? atoi(argv[1]) : 150;
  int frames = argc > 2 ? atoi(argv[2]) : 320;
  int warm = 20;
  int f, widgets = 0;
  double build_ns = 0, render_ns = 0;
  size_t cmd_bytes = 0;
  unsigned long total_cmds = 0, total_batches = 0;
  char name[256];
  FILE *fp;

  if (rows > MAXR) { rows = MAXR; }

  init_scene();
  mu_init(&ctx);
  ctx.text_width = text_width_cb;
  ctx.text_height = text_height_cb;

  sprintf(name, "out_%s_%d.hashes", TAG, rows);
  fp = fopen(name, "w");
  if (!fp) { fprintf(stderr, "cannot open %s\n", name); return 1; }

  for (f = 0; f < warm + frames; f++) {
    double t0, t1, t2;
    /* deterministic input: wandering mouse + periodic scroll */
    mu_input_mousemove(&ctx, 60 + (f * 37) % 1100, 40 + (f * 53) % 640);
    if (f % 3 == 0) { mu_input_scroll(&ctx, 0, -30); }

    t0 = now_ns();
    mu_begin(&ctx);
    widgets = build_scene(&ctx, rows);
    mu_end(&ctx);
    t1 = now_ns();

    /* clear + consume command list */
    {
      uint32_t bg = 0xff1e1e1eu;
      size_t i, n = FBW * FBH;
      for (i = 0; i < n; i++) { fb[i] = bg; }
    }
    stat_cmds = 0;
    stat_batches = 0;
    render(&ctx);
    t2 = now_ns();

    if (f >= warm) {
      build_ns += t1 - t0;
      render_ns += t2 - t1;
      cmd_bytes += ctx.command_list.idx;
      total_cmds += stat_cmds;
      total_batches += stat_batches;
    }
    fprintf(fp, "%d %016llx\n", f, (unsigned long long) hash_fb());
  }
  fclose(fp);

  sprintf(name, "out_%s_%d.rgba", TAG, rows);
  fp = fopen(name, "wb");
  if (!fp) { fprintf(stderr, "cannot open %s\n", name); return 1; }
  fwrite(fb, 1, sizeof(fb), fp);
  fclose(fp);

  printf("tag=%s rows=%d widgets=%d frames=%d\n", TAG, rows, widgets, frames);
  printf("build_avg_ms=%.4f render_avg_ms=%.4f\n",
    build_ns / frames / 1e6, render_ns / frames / 1e6);
  printf("cmd_bytes_avg=%.0f cmds_avg=%.1f batches_avg=%.1f\n",
    (double) cmd_bytes / frames,
    (double) total_cmds / frames, (double) total_batches / frames);
  return 0;
}
