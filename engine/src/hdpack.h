#ifndef REMASTERIA_HDPACK_H
#define REMASTERIA_HDPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "motion.h"

/* HD art: every tile and sprite of the game, redrawn at 4x (tools/remasteria hdpack), drawn by the
 * engine in place of the original one. Each pixel of the frame knows which tile, palette and texel
 * it came from (CoreMotion), so the new art follows the game exactly: scrolling, animation, flips,
 * palette and brightness changes. Tiles the pack doesn't have keep the original pixels.
 *
 * A tile is identified by its graphics data and its palette colors (hdpack_key), so the same
 * drawing in another palette is another tile. */

#define HDPACK_SCALE 4

bool hdpack_load(const char *path);
bool hdpack_loaded(void);
/* The frame (XRGB8888, 256 wide, cropped `m->top` rows from the core's) redrawn at 4x with the HD
 * art, or NULL when no tile of it is in the pack. */
const uint32_t *hdpack_render(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m,
	unsigned *width, unsigned *out_height);

/* Art capture: while the game is played, saves to `dir` the frames that show tiles (or parts of
 * tiles) not seen before, clean (full brightness, no color math), for the pack builder. */
void hdpack_capture_init(const char *dir);
void hdpack_capture(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m);
void hdpack_capture_save(void);

#endif
