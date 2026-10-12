#include "motion.h"

#include <stdlib.h>
#include <string.h>

#define WIDTH 256
#define MAX_HEIGHT 240
#define MAX_STEP 24 /* pixels per frame; a bigger jump is a cut, not motion */
#define NO_MATCH 0xff
#define BACKWARD 0x80 /* depth flag: pixel filled from the newer frame */

typedef struct {
	bool ok;
	unsigned height;
	uint32_t pixels[MAX_HEIGHT * WIDTH];
	CoreMotion motion;
} Frame;

static const CoreMotion *(*core_motion)(void);
static Frame frames[2];
static int latest;
static unsigned captured;
static uint32_t between[MAX_HEIGHT * 2 * WIDTH * 2];
static uint8_t depth[MAX_HEIGHT * 2 * WIDTH * 2];

void motion_init(const CoreMotion *(*source)(void))
{
	core_motion = source;
}

bool motion_available(void)
{
	return core_motion != NULL;
}

void motion_capture(const void *pixels, unsigned width, unsigned height, size_t pitch)
{
	if (!core_motion)
		return;
	latest ^= 1;
	if (captured < 2)
		captured++;
	Frame *f = &frames[latest];
	const CoreMotion *m = core_motion();
	f->ok = m && m->valid && width == WIDTH && height <= MAX_HEIGHT && height + m->top <= 240;
	if (!f->ok)
		return;
	f->height = height;
	for (unsigned y = 0; y < height; y++)
		memcpy(&f->pixels[y * WIDTH], (const char *)pixels + y * pitch, WIDTH * sizeof(uint32_t));
	memcpy(&f->motion, m, sizeof *m);
}

static int wrap(int delta, int range)
{
	delta &= range - 1;
	return delta >= range / 2 ? delta - range : delta;
}

/* Which object of `b` each object of `a` became: same palette and size, closest position,
 * preferring the same tile and the same OAM slot. */
static void match_objects(const CoreMotion *a, const CoreMotion *b, uint8_t *match)
{
	for (int i = 0; i < 128; i++) {
		int best = NO_MATCH, best_score = 1 << 30;
		for (int j = 0; j < 128; j++) {
			if (b->objects[j].size != a->objects[i].size || (b->objects[j].attributes & 0x0f) != (a->objects[i].attributes & 0x0f))
				continue;
			int dx = wrap(b->objects[j].x - a->objects[i].x, 512), dy = wrap(b->objects[j].y - a->objects[i].y, 256);
			if (abs(dx) > MAX_STEP || abs(dy) > MAX_STEP)
				continue;
			int score = (abs(dx) + abs(dy)) * 4 + (b->objects[j].character != a->objects[i].character ? 16 : 0) + (j != i);
			if (score < best_score)
				best = j, best_score = score;
		}
		match[i] = (uint8_t)best;
	}
}

/* How far the pixel at (x, row) of `a` moves to reach its place in `b`. */
static void pixel_motion(const CoreMotion *a, const CoreMotion *b, const uint8_t *match, int row, int x, int *dx, int *dy)
{
	*dx = *dy = 0;
	int source = a->source[row][x];
	if (source < 4) {
		if (a->mode[row] != b->mode[row] || a->mode[row] == 7)
			return;
		*dx = wrap(a->hoffset[row][source] - b->hoffset[row][source], 1024);
		*dy = wrap(a->voffset[row][source] - b->voffset[row][source], 1024);
	} else if (source < 6) {
		int i = a->object[row][x];
		if (i >= 128 || match[i] == NO_MATCH)
			return;
		*dx = wrap(b->objects[match[i]].x - a->objects[i].x, 512);
		*dy = wrap(b->objects[match[i]].y - a->objects[i].y, 256);
	}
	if (abs(*dx) > MAX_STEP || abs(*dy) > MAX_STEP)
		*dx = *dy = 0;
}

/* Moves every pixel of `f` halfway towards `other`, drawing 2x2 blocks with priority. The newer
 * frame only fills what the older one leaves uncovered. */
static void splat(const Frame *f, const Frame *other, const uint8_t *match, bool backward)
{
	const CoreMotion *a = &f->motion, *b = &other->motion;
	int out_w = WIDTH * 2, out_h = (int)f->height * 2;
	for (unsigned y = 0; y < f->height; y++) {
		int row = (int)y + a->top;
		for (int x = 0; x < WIDTH; x++) {
			int dx, dy;
			pixel_motion(a, b, match, row, x, &dx, &dy);
			int tx = 2 * x + dx, ty = 2 * (int)y + dy;
			uint8_t z = (uint8_t)((a->priority[row][x] & 0x7f) + 1);
			uint32_t color = f->pixels[y * WIDTH + x];
			for (int j = 0; j < 2; j++)
				for (int i = 0; i < 2; i++) {
					int px = tx + i, py = ty + j;
					if (px < 0 || py < 0 || px >= out_w || py >= out_h)
						continue;
					size_t at = (size_t)py * out_w + px;
					bool draw = backward ? depth[at] == 0 || ((depth[at] & BACKWARD) && z > (depth[at] & 0x7f)) : z > depth[at];
					if (draw) {
						between[at] = color;
						depth[at] = backward ? z | BACKWARD : z;
					}
				}
		}
	}
}

const uint32_t *motion_between(unsigned *width, unsigned *height)
{
	const Frame *old = &frames[latest ^ 1], *now = &frames[latest];
	if (captured < 2 || !old->ok || !now->ok || old->height != now->height || old->motion.top != now->motion.top
		|| now->motion.frame != old->motion.frame + 1)
		return NULL;

	uint8_t forward[128], backward[128];
	match_objects(&old->motion, &now->motion, forward);
	match_objects(&now->motion, &old->motion, backward);

	unsigned out_w = WIDTH * 2, out_h = old->height * 2;
	memset(depth, 0, (size_t)out_w * out_h);
	splat(old, now, forward, false);
	splat(now, old, backward, true);
	for (unsigned y = 0; y < out_h; y++)
		for (unsigned x = 0; x < out_w; x++)
			if (!depth[y * out_w + x])
				between[y * out_w + x] = old->pixels[(y / 2) * WIDTH + x / 2];
	*width = out_w;
	*height = out_h;
	return between;
}

const uint32_t *motion_current(unsigned *width, unsigned *height)
{
	const Frame *now = &frames[latest];
	if (!captured || !now->ok)
		return NULL;
	*width = WIDTH;
	*height = now->height;
	return now->pixels;
}
