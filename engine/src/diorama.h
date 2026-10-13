#ifndef REMASTERIA_DIORAMA_H
#define REMASTERIA_DIORAMA_H

#include <stddef.h>
#include <stdint.h>

#include "motion.h"

/* Diorama look: the room is a board seen from a little lower than the game's top-down view, the
 * characters stand on it as cutouts, and the windows stay flat in front. This splits a frame into
 * those layers, in console pixels; video.c draws them. The picture itself is the remastered one,
 * so light, shadows and hard pixels carry over. */
#define DIORAMA_SPRITES 96
#define DIORAMA_FLAT 255   /* id of window and HUD pixels */

typedef struct {
	float x0, y0, x1, y1;  /* box in console pixels; y1 is the row under the feet */
	float dx, dy;          /* how far it moved since the frame before */
} DioramaSprite;

typedef struct {
	unsigned height;
	/* 0xAARRGGBB. A 255: a sprite or window pixel; the board shows the scenery under it, lit
	 * like the scenery around it. A 0: the picture. */
	uint32_t under[240 * 256];
	uint8_t id[240 * 256];  /* sprite index + 1, DIORAMA_FLAT, or 0 for the board */
	float shift[240][2];    /* how far the board's content moved on each row since the frame before */
	int sprites;
	DioramaSprite sprite[DIORAMA_SPRITES];
} Diorama;

/* What the core's retro_remasteria_ground() points at (tools/remasteria/patches/bsnes-hd.patch). */
typedef struct {
	uint32_t color[240][256];    /* the final color with sprites left out */
	uint8_t source[240][256];
	uint32_t scenery[240][256];  /* the same without the third layer, where windows are drawn */
} CoreGround;

/* `ground` is the core's CoreGround for the frame. `hd` is the picture shown, at `scale` (1 for
 * the console picture). */
const Diorama *diorama_build(const CoreMotion *m, const CoreGround *ground, const void *console, size_t pitch,
	unsigned height, const uint32_t *hd, int scale);
void diorama_reset(void);

#endif
