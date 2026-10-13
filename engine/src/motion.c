#include "motion.h"

#include <stdlib.h>
#include <string.h>

#include "remaster.h"

#define WIDTH 256
#define MAX_HEIGHT 240
#define MAX_STEP 24 /* pixels per frame; a bigger jump is a cut, not motion */
#define NO_MATCH 0xff
#define BACKWARD 0x80 /* depth flag: pixel filled from the newer frame */
#define HD REMASTER_SCALE_MAX /* storage; each frame records the scale it was actually drawn at */

typedef struct {
	bool ok, has_hd;
	unsigned height, scale;
	uint32_t pixels[MAX_HEIGHT * WIDTH];
	uint32_t hd[MAX_HEIGHT * HD * WIDTH * HD];
	CoreMotion motion;
} Frame;

static const CoreMotion *(*core_motion)(void);
static Frame frames[2];
static int latest;
static unsigned captured;
static uint32_t between[MAX_HEIGHT * HD * WIDTH * HD];
static uint8_t depth[MAX_HEIGHT * HD * WIDTH * HD];

void motion_init(const CoreMotion *(*source)(void))
{
	core_motion = source;
}

bool motion_available(void)
{
	return core_motion != NULL;
}

void motion_reset(void)
{
	captured = 0;
	latest = 0;
	frames[0].ok = frames[1].ok = false;
}

void motion_capture(const void *pixels, unsigned width, unsigned height, size_t pitch, const uint32_t *hd,
	unsigned hd_scale)
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
	f->has_hd = hd != NULL && hd_scale >= 1 && hd_scale <= HD;
	f->scale = f->has_hd ? hd_scale : 0;
	if (f->has_hd)
		memcpy(f->hd, hd, (size_t)WIDTH * hd_scale * height * hd_scale * sizeof(uint32_t));
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

static bool can_draw(uint8_t d, uint8_t z, bool backward)
{
	if (!backward)
		return z > d;
	return d == 0 || ((d & BACKWARD) && z > (d & 0x7f));
}

/* One console pixel, shifted halfway, as a scale x scale block. Used when the pixel moves
 * vertically: a horizontal run is copied with memcpy instead. */
static void splat_pixel(const Frame *f, int scale, int out_w, int y0, int y1, int y, int x, int dx, int dy, uint8_t z, bool backward)
{
	int tx = scale * x + dx * scale / 2, ty = scale * y + dy * scale / 2;
	uint8_t drawn = backward ? (uint8_t)(z | BACKWARD) : z;
	uint32_t color = f->pixels[y * WIDTH + x];
	for (int j = 0; j < scale; j++)
		for (int i = 0; i < scale; i++) {
			int px = tx + i, py = ty + j;
			if (px < 0 || py < y0 || px >= out_w || py >= y1)
				continue;
			size_t at = (size_t)py * out_w + px;
			if (!can_draw(depth[at], z, backward))
				continue;
			between[at] = f->has_hd ? f->hd[(size_t)(scale * y + j) * out_w + scale * x + i] : color;
			depth[at] = drawn;
		}
}

/* Copies a horizontal run of console pixels already shifted by dx. Long stretches of one
 * layer (the usual camera scroll) move with memcpy; only the columns a nearer sprite already
 * claimed are left in place. */
static void splat_run(const Frame *f, int scale, int out_w, int y0, int y1, int y, int x0, int x1, int dx, uint8_t z, bool backward, bool fresh, bool uniform)
{
	int shift = dx * scale / 2;
	int base_y = y * scale;
	int src0 = x0 * scale;
	int dst0 = src0 + shift;
	int n = (x1 - x0) * scale;
	int src_skip = 0;
	if (dst0 < 0) {
		src_skip = -dst0;
		n += dst0;
		dst0 = 0;
	}
	if (dst0 + n > out_w)
		n = out_w - dst0;
	if (n <= 0)
		return;
	uint8_t drawn = backward ? (uint8_t)(z | BACKWARD) : z;
	int first = base_y < y0 ? y0 - base_y : 0, last = base_y + scale > y1 ? y1 - base_y : scale;
	if (first >= last)
		return;
	if (first > 0 || last < scale)
		uniform = false;
	if (!backward && fresh) {
		for (int j = first; j < last; j++) {
			size_t at = (size_t)(base_y + j) * out_w + dst0;
			memcpy(&between[at], &f->hd[(size_t)(base_y + j) * out_w + src0 + src_skip], (size_t)n * sizeof(uint32_t));
			memset(&depth[at], drawn, (size_t)n);
		}
		return;
	}
	int subrows = uniform ? 1 : last;
	for (int j = first; j < subrows; j++) {
		uint8_t *drow = &depth[(size_t)(base_y + j) * out_w + dst0];
		int i = 0;
		while (i < n) {
			bool ok = can_draw(drow[i], z, backward);
			int start = i;
			while (i < n && can_draw(drow[i], z, backward) == ok)
				i++;
			if (!ok)
				continue;
			int len = i - start;
			int apply = uniform ? scale : 1;
			for (int k = 0; k < apply; k++) {
				size_t at = (size_t)(base_y + j + k) * out_w + dst0 + start;
				memcpy(&between[at], &f->hd[(size_t)(base_y + j + k) * out_w + src0 + src_skip + start], (size_t)len * sizeof(uint32_t));
				memset(&depth[at], drawn, (size_t)len);
			}
		}
	}
}

/* Moves every pixel of `f` halfway towards `other`, drawing scale x scale blocks with priority
 * (the pixel's color, or its block of HD art). The newer frame only fills what the older one
 * leaves uncovered. */
static void splat(const Frame *f, const Frame *other, const uint8_t *match, bool backward, int scale, int y0, int y1)
{
	const CoreMotion *a = &f->motion, *b = &other->motion;
	int out_w = WIDTH * scale;
	/* Only console rows that can reach screen rows y0..y1. A band that starts below the top
	 * cannot know whether a row above it moved into its rows, so it never assumes them empty. */
	int from = y0 / scale - MAX_STEP / 2 - 1, to = (y1 - 1) / scale + MAX_STEP / 2 + 2;
	if (from < 0)
		from = 0;
	if (to > (int)f->height)
		to = (int)f->height;
	bool crossed = y0 > 0; /* a vertically moving pixel can land on other rows */
	for (unsigned y = (unsigned)from; y < (unsigned)to; y++) {
		int row = (int)y + a->top;
		bool fresh = !crossed;
		for (int x = 0; x < WIDTH;) {
			int dx, dy;
			pixel_motion(a, b, match, row, x, &dx, &dy);
			uint8_t z = (uint8_t)((a->priority[row][x] & 0x7f) + 1);
			int x1 = x + 1;
			/* Consecutive pixels of one layer share the scroll and the priority, so the
			 * whole run is one rectangle. A vertical step stays per pixel. */
			if (dy == 0 && f->has_hd)
				while (x1 < WIDTH) {
					int dx2, dy2;
					pixel_motion(a, b, match, row, x1, &dx2, &dy2);
					uint8_t z2 = (uint8_t)((a->priority[row][x1] & 0x7f) + 1);
					if (dx2 != dx || dy2 != 0 || z2 != z)
						break;
					x1++;
				}
			if (dx == 0 && dy == 0 && backward) {
				x = x1;
				fresh = false;
				continue;
			}
			if (dy == 0 && f->has_hd)
				splat_run(f, scale, out_w, y0, y1, (int)y, x, x1, dx, z, backward, fresh, !crossed);
			else {
				if (dy != 0)
					crossed = true;
				for (int xx = x; xx < x1; xx++) {
					int pdx, pdy;
					pixel_motion(a, b, match, row, xx, &pdx, &pdy);
					splat_pixel(f, scale, out_w, y0, y1, (int)y, xx, pdx, pdy, z, backward);
				}
			}
			fresh = false;
			x = x1;
		}
	}
}

typedef struct {
	const Frame *old, *now;
	const uint8_t *forward, *backward;
	int scale;
} Between;

static void between_rows(void *context, int y0, int y1);

const uint32_t *motion_between(unsigned *width, unsigned *height)
{
	const Frame *old = &frames[latest ^ 1], *now = &frames[latest];
	if (captured < 2 || !old->ok || !now->ok || old->height != now->height || old->motion.top != now->motion.top
		|| now->motion.frame != old->motion.frame + 1)
		return NULL;

	uint8_t forward[128], backward[128];
	match_objects(&old->motion, &now->motion, forward);
	match_objects(&now->motion, &old->motion, backward);

	int scale;
	if (old->has_hd && now->has_hd) {
		if (old->scale != now->scale)
			return NULL;
		scale = (int)old->scale;
	} else if (!old->has_hd && !now->has_hd) {
		scale = 2;
	} else {
		return NULL;
	}
	Between job = { old, now, forward, backward, scale };
	remaster_bands((int)old->height * scale, between_rows, &job);
	*width = WIDTH * scale;
	*height = old->height * scale;
	return between;
}

static void between_rows(void *context, int y0, int y1)
{
	const Between *job = context;
	const Frame *old = job->old;
	int scale = job->scale;
	unsigned out_w = WIDTH * scale;
	memset(&depth[(size_t)y0 * out_w], 0, (size_t)out_w * (unsigned)(y1 - y0));
	splat(old, job->now, job->forward, false, scale, y0, y1);
	splat(job->now, old, job->backward, true, scale, y0, y1);
	for (unsigned y = (unsigned)y0; y < (unsigned)y1; y++) {
		const uint32_t *src = old->has_hd ? &old->hd[y * out_w] : NULL;
		unsigned x = 0;
		while (x < out_w) {
			if (depth[y * out_w + x]) {
				x++;
				continue;
			}
			unsigned start = x;
			while (x < out_w && !depth[y * out_w + x])
				x++;
			if (src)
				memcpy(&between[y * out_w + start], &src[start], (size_t)(x - start) * sizeof(uint32_t));
			else
				for (unsigned i = start; i < x; i++)
					between[y * out_w + i] = old->pixels[(y / 2) * WIDTH + i / 2];
		}
	}
}

const uint32_t *motion_current(unsigned *width, unsigned *height)
{
	const Frame *now = &frames[latest];
	if (!captured || !now->ok)
		return NULL;
	int scale = now->has_hd ? (int)now->scale : 1;
	*width = WIDTH * scale;
	*height = now->height * scale;
	return now->has_hd ? now->hd : now->pixels;
}
