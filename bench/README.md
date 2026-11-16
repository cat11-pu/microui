# UI command-build optimization benchmark

Benchmark and pixel-comparison harness for the `src/microui.c`
command-generation optimizations.

## What it does

`bench.c` builds a scene of 4 windows x N control rows (button + slider +
checkbox + label per row, plus textbox/header/treenodes/panel chrome). Most
rows are scrolled out of view, matching the "many controls, few visible"
in-game UI case. It measures:

- `build_avg_ms` — time to rebuild the draw-command list
  (`mu_begin` .. `mu_end`), the per-frame cost being optimized
- `render_avg_ms` — time to consume the command list with a deterministic
  software renderer (src-over blending, same as `demo/renderer.c`'s GL
  blend func)
- command-stream stats: bytes, command count, material batches (runs of
  same type+color commands)

Every frame is rasterized and hashed (FNV-1a over the framebuffer); the
final frame is also written out raw. `run.sh` compares the baseline
(microui.c from git HEAD) against the optimized `src/microui.c` and fails
if any pixel differs.

## Reproduce

```sh
bench/run.sh
```

Builds `bench_base` / `bench_opt` with `gcc -O2`, runs 320 frames per tier
(+20 warmup) with deterministic simulated input (wandering mouse, periodic
scroll).

## Results (gcc 15.2, -O2, this machine)

| widgets | baseline build | optimized build | speedup |
|--------:|---------------:|----------------:|--------:|
|     672 |       0.058 ms |        0.035 ms |  -40 %  |
|    2432 |       0.179 ms |        0.066 ms |  -63 %  |
|    6432 |       0.458 ms |        0.127 ms |  -72 %  |

Command stream per frame: 1790 -> 1786 commands, 854 -> 850 material
batches, 53068 -> 52956 bytes (alpha-0 panel backgrounds and redundant
clip commands eliminated; fully clipped controls emit nothing).

Pixel comparison: 340/340 frames identical in all three tiers (per-frame
hashes + final-frame `cmp` of the raw RGBA dump).

## Optimizations in src/microui.c

- **Skip invisible subtrees**: `mu_draw_control_frame`,
  `mu_draw_control_text`, `mu_textbox_raw`, `mu_slider_ex` and
  `mu_number_ex` early-out when the control rect (plus 1px border) is fully
  outside the current clip rect. Interaction state (`mu_update_control`)
  still runs, so hover/focus behavior is unchanged; clipped sliders/numbers
  also skip their per-frame `sprintf`.
- **Less temporary work**: `mu_draw_control_text` passes its already
  computed text width to the emitter instead of measuring the string twice;
  `mu_draw_rect` takes a fast path when the rect is fully inside the clip
  rect (no intersection math).
- **Command merging**: `mu_draw_rect` drops fully transparent rects
  (alpha 0 is a no-op under src-over blending, e.g. `MU_COLOR_PANELBG`);
  `mu_set_clip` merges a clip command identical to the previous one.
