#ifndef REMASTERIA_MOTION_H
#define REMASTERIA_MOTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What each visible pixel of the last frame is made of, exported by the patched bsnes-hd core
 * (retro_remasteria_motion; tools/remasteria/patches/bsnes-hd.patch). Same layout as there. */
typedef struct {
	uint32_t frame;
	uint8_t valid;              /* false for HD (mode 7), hires and interlaced frames */
	uint8_t top;                /* rows the core crops from the top of its output */
	uint8_t source[240][256];   /* 0-3 BG1-BG4, 4-5 OBJ, 6 backdrop */
	uint8_t priority[240][256];
	uint8_t object[240][256];   /* OAM index of OBJ pixels */
	uint8_t mode[240];          /* BG mode of each line; 0xff when blank */
	uint16_t hoffset[240][4];
	uint16_t voffset[240][4];
	struct { uint16_t x; uint8_t y, character, attributes, size; } objects[128];
} CoreMotion;

/* In-between frames: the game runs at 60 fps; on a 120 Hz display the engine shows, between two
 * game frames, one where every layer and every sprite is halfway along its real motion (camera
 * scroll per line, sprite positions from OAM). No guessing as in TV motion smoothing: the motion
 * comes from the console's own state, so there is no blur and no warping artifacts. */
void motion_init(const CoreMotion *(*source)(void));
bool motion_available(void);
/* Call with every frame the core outputs (XRGB8888). */
void motion_capture(const void *pixels, unsigned width, unsigned height, size_t pitch);
/* The halfway frame between the last two captured frames, at twice the resolution, or NULL when
 * they can't be interpolated (scene cut, HD mode 7, other frame sizes). */
const uint32_t *motion_between(unsigned *width, unsigned *height);
/* The last captured frame, to show after the halfway one. */
const uint32_t *motion_current(unsigned *width, unsigned *height);

#endif
