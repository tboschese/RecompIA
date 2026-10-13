#ifndef REMASTERIA_REMASTER_H
#define REMASTERIA_REMASTER_H

#include <stddef.h>
#include <stdint.h>

#include "motion.h"

/* The engine's remaster pass. Any game whose project sets remaster=on and whose core
 * exports CoreMotion is redrawn from the console frame itself, on the largest integer
 * grid that fits the screen. Every console pixel is the same number of screen pixels.
 * Flat areas keep the original color. Light is a hard flat step of that same color,
 * not a new one: the inside of a body's top third is a little lighter, the inside
 * of the bottom third a little darker. Black stays black. The middle stays the
 * drawn color. A one-pixel stroke, such as a
 * letter, is not rewritten. The outline itself stays the color the game drew,
 * so a light step does not trace the stair. A 45° step of a warm shape against a
 * flat sky or a flat black field is redrawn inside that same cell as a one-pixel
 * stair, copying the color already on the outside. A square corner stays square.
 * A stippled cloud stays
 * the drawing. The cell stays where the game put it.
 * Also: a contact shadow, one flat step darker in the same tone, sits under
 * sprites. Gameplay stays put: nothing is shifted. */
#define REMASTER_SCALE 6
#define REMASTER_SCALE_MAX 16

/* XRGB8888, 256*REMASTER_SCALE wide. NULL when this frame has no per-pixel source
 * (HD Mode 7, hires, interlace): those stay on the core's own picture. */
const uint32_t *remaster_frame(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m,
	unsigned *width, unsigned *out_height);
/* The scale the next remaster_frame draws at; 0 asks the window. Set it before running the
 * remaster off the main thread, which must not ask the window. */
void remaster_use_scale(int scale);

/* Dynamic light. A game's lights.txt (tools/remasteria lights) names its lights by tile, with
 * the key the HD packs use: one line per tile, "key kind luma [r g b radius]". kind is flame
 * (flickers), glow, window (daylight falls into the room below it) or none (never a light);
 * ? is a tile not reviewed yet, and is ignored. Pixels of a listed tile glow from `luma` up.
 * Tiles not listed are left to detection by look, which a game can turn off. */
bool remaster_load_lights(const char *path);
void remaster_detect_lights(bool on);

/* Lights detection found in the last frame, one entry per tile of each, for review. Only
 * kept while `remaster_find_lights(true)`. */
typedef struct {
	uint64_t key;
	char kind;        /* 'f' flame, 'g' glow, 'w' window */
	uint8_t luma;     /* dimmest glowing pixel of the tile */
	uint8_t r, g, b;
	int radius;
	int x0, y0, x1, y1;  /* the whole light, in console pixels */
} LightFound;
void remaster_find_lights(bool on);
const LightFound *remaster_lights_found(int *count);

/* Runs `rows(context, y0, y1)` over 0..height in bands on every core. Only for work where a
 * band writes nothing outside its own rows and reads nothing another band writes. */
typedef void RowsFn(void *context, int y0, int y1);
void remaster_bands(int height, RowsFn *rows, void *context);

#endif
