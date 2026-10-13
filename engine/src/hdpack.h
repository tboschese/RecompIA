#ifndef REMASTERIA_HDPACK_H
#define REMASTERIA_HDPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "motion.h"

/* HD art: the game's own tiles and sprites, redrawn as crisp 2x pixel art in their original colors
 * (tools/remasteria hdpack). The menu switches between that and the original console picture.
 * Each pixel of the frame knows which tile, palette and texel it came from (CoreMotion), so the
 * redraw follows the game exactly. Tiles the pack doesn't have keep the original pixels. */

#define HDPACK_SCALE 2

/* Appends one pack. False when the file is missing or not a pack of this scale. */
bool hdpack_load(const char *path);
bool hdpack_loaded(void);
/* Cycles the selected style, and past the last one back to the original graphics. */
void hdpack_next(void);
/* Picks "remaster" or "original". */
void hdpack_use(const char *id);
const char *hdpack_style_name(void);
/* The frame (XRGB8888, 256 wide, cropped `m->top` rows from the core's) redrawn at 2x with the
 * selected style, or NULL when the original graphics are selected or no tile of it is in the pack. */
const uint32_t *hdpack_render(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m,
	unsigned *width, unsigned *out_height);

/* The identity of a pixel of the frame (`row` counts from the top of the screen): its tile's graphics
 * and palette, the key the packs use. 0 where a pixel has none (backdrop, mode 7, direct color).
 * hdpack_keys_begin() once per frame, before the first. */
void hdpack_keys_begin(void);
uint64_t hdpack_key(const CoreMotion *m, int row, int x);

/* Art capture: while the game is played, saves to `dir` the frames that show tiles (or parts of
 * tiles) not seen before, clean (full brightness, no color math), for the pack builder. */
void hdpack_capture_init(const char *dir);
void hdpack_capture(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m);
void hdpack_capture_save(void);

#endif
