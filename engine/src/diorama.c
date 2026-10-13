#include "diorama.h"

#include "remaster.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 256
#define MAX_HEIGHT 240
#define MAX_STEP 24 /* pixels per frame; a bigger jump is a cut, not motion */

static Diorama frame;
static struct {
	bool ok;
	uint32_t number;
	uint8_t mode[MAX_HEIGHT];
	uint16_t hoffset[MAX_HEIGHT][4], voffset[MAX_HEIGHT][4];
	CoreObject objects[128];
} before;

void diorama_reset(void)
{
	before.ok = false;
}

static int wrap(int delta, int range)
{
	delta &= range - 1;
	return delta >= range / 2 ? delta - range : delta;
}

static bool is_sprite(uint8_t source)
{
	return source == 4 || source == 5;
}

/* Windows are drawn on mode 1's third layer. Some rooms draw scenery there too (tree tops, fog);
 * that part scrolls with the room. As in remaster.c: the room is the scroll the layer has on most
 * of the lines where it shows, and a layer that shows only in a band of a third of the screen is
 * a window. */
static void find_windows(const CoreMotion *m, unsigned height)
{
	uint32_t keys[MAX_HEIGHT];
	int counts[MAX_HEIGHT], first[MAX_HEIGHT], last[MAX_HEIGHT], distinct = 0;
	for (unsigned y = 0; y < height; y++) {
		int row = (int)y + m->top, shown = 0;
		for (int x = 0; x < WIDTH; x += 8)
			shown += m->source[row][x] == 2;
		if (!shown)
			continue;
		uint32_t key = (uint32_t)m->hoffset[row][2] << 16 | m->voffset[row][2];
		int k = 0;
		while (k < distinct && keys[k] != key)
			k++;
		if (k == distinct) {
			keys[distinct] = key;
			first[distinct] = (int)y;
			counts[distinct++] = 0;
		}
		counts[k] += shown;
		last[k] = (int)y;
	}
	int best = 0;
	for (int k = 1; k < distinct; k++)
		if (counts[k] > counts[best])
			best = k;
	bool band = distinct && (last[best] - first[best] + 1) * 3 <= (int)height;
	for (unsigned y = 0; y < height; y++) {
		int row = (int)y + m->top;
		if (m->mode[row] != 1)
			continue;
		uint32_t key = (uint32_t)m->hoffset[row][2] << 16 | m->voffset[row][2];
		bool window = band || !distinct || key != keys[best];
		for (int x = 0; x < WIDTH && window; x++)
			if (m->source[row][x] == 2)
				frame.id[y * WIDTH + x] = DIORAMA_FLAT;
	}
}

static uint32_t pixel_at(const void *console, size_t pitch, int x, int y)
{
	return ((const uint32_t *)((const char *)console + (size_t)y * pitch))[x] & 0xffffff;
}

static uint32_t relight(uint32_t color, uint32_t was, uint32_t lit)
{
	uint32_t result = 0;
	for (int shift = 0; shift <= 16; shift += 8) {
		int a = was >> shift & 255, b = lit >> shift & 255, c = color >> shift & 255;
		int v = a ? c * b / a : c;
		result |= (uint32_t)(v > 255 ? 255 : v) << shift;
	}
	return result;
}

/* Scenery hidden under a sprite or a window, in the light the remaster gave the nearest scenery
 * on its row, or the light of the whole room when there is none near. */
static uint32_t lit(uint32_t color, const void *console, size_t pitch, const uint32_t *hd, int scale, int x, int y,
	uint32_t room_was, uint32_t room_lit)
{
	color &= 0xffffff;
	if (!hd || scale < 1)
		return color;
	int near = -1;
	for (int d = 1; d < 24 && near < 0; d++) {
		if (x - d >= 0 && frame.id[y * WIDTH + x - d] == 0)
			near = x - d;
		else if (x + d < WIDTH && frame.id[y * WIDTH + x + d] == 0)
			near = x + d;
	}
	if (near < 0)
		return relight(color, room_was, room_lit);
	return relight(color, pixel_at(console, pitch, near, y),
		hd[(size_t)(y * scale + scale / 2) * WIDTH * scale + near * scale + scale / 2]);
}

const Diorama *diorama_build(const CoreMotion *m, const CoreGround *ground, const void *console, size_t pitch,
	unsigned height, const uint32_t *hd, int scale)
{
	if (!m || !ground || !console || height > MAX_HEIGHT || height + m->top > MAX_HEIGHT)
		return NULL;
	/* A screen that is mostly window (a menu, the name screen) has no room to lay down. */
	int windows = 0;
	for (unsigned y = 0; y < height; y++)
		for (int x = 0; x < WIDTH && m->mode[y + m->top] == 1; x += 2)
			windows += m->source[y + m->top][x] == 2;
	if (windows * 2 * 5 > (int)height * WIDTH * 2) {
		before.ok = false;
		return NULL;
	}
	frame.height = height;
	bool follows = before.ok && m->frame == before.number + 1;

	memset(frame.id, 0, sizeof frame.id);
	find_windows(m, height);
	/* Letters outside the windows: a menu screen drawn on the other layers (the name screen). */
	int letters = 0;
	for (unsigned y = 0; hd && y < height; y++)
		for (int x = 0; x < WIDTH; x++)
			letters += remaster_glyph(x, (int)y) && frame.id[y * WIDTH + x] != DIORAMA_FLAT;
	if (letters > 600) {
		before.ok = false;
		return NULL;
	}

	/* One cutout per connected sprite shape of one palette (a game gives each character its own;
	 * a party walking in a line touches); one touching a window belongs to the window. */
	static int stack[WIDTH * MAX_HEIGHT], members[WIDTH * MAX_HEIGHT];
	static uint8_t seen[MAX_HEIGHT][WIDTH];
	memset(seen, 0, sizeof seen);
	uint8_t match[128];
	if (follows)
		motion_match_objects(m->objects, before.objects, match);
	frame.sprites = 0;
	for (unsigned y = 0; y < height; y++)
		for (int x = 0; x < WIDTH; x++) {
			int row = (int)y + m->top;
			if (seen[y][x] || !is_sprite(m->source[row][x]) || frame.id[y * WIDTH + x])
				continue;
			int top = 0, count = 0, x0 = x, x1 = x, y0 = (int)y, y1 = (int)y;
			bool touches_window = false;
			int objects[128] = { 0 };
			stack[top++] = (int)y * WIDTH + x;
			seen[y][x] = 1;
			while (top > 0) {
				int at = stack[--top], cy = at / WIDTH, cx = at % WIDTH;
				members[count++] = at;
				uint8_t object = m->object[cy + m->top][cx];
				if (object < 128)
					objects[object]++;
				if (cx < x0) x0 = cx;
				if (cx > x1) x1 = cx;
				if (cy < y0) y0 = cy;
				if (cy > y1) y1 = cy;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int nx = cx + dx, ny = cy + dy;
						if (nx < 0 || ny < 0 || nx >= WIDTH || ny >= (int)height || seen[ny][nx])
							continue;
						if (frame.id[ny * WIDTH + nx] == DIORAMA_FLAT)
							touches_window = true;
						if (!is_sprite(m->source[ny + m->top][nx]) || frame.id[ny * WIDTH + nx]
							|| m->palette[ny + m->top][nx] != m->palette[cy + m->top][cx])
							continue;
						seen[ny][nx] = 1;
						stack[top++] = ny * WIDTH + nx;
					}
			}
			uint8_t id = touches_window ? DIORAMA_FLAT : 0;
			if (!touches_window && frame.sprites < DIORAMA_SPRITES) {
				DioramaSprite *s = &frame.sprite[frame.sprites++];
				*s = (DioramaSprite){ (float)x0, (float)y0, (float)x1 + 1, (float)y1 + 1, 0, 0 };
				int main = -1;
				for (int i = 0; i < 128; i++)
					if (objects[i] && (main < 0 || objects[i] > objects[main]))
						main = i;
				if (follows && main >= 0 && match[main] < 128) {
					int dx = wrap(m->objects[main].x - before.objects[match[main]].x, 512);
					int dy = wrap(m->objects[main].y - before.objects[match[main]].y, 256);
					if (abs(dx) <= MAX_STEP && abs(dy) <= MAX_STEP)
						s->dx = (float)dx, s->dy = (float)dy;
				}
				id = (uint8_t)frame.sprites;
			}
			for (int i = 0; i < count; i++)
				frame.id[members[i]] = id;
		}

	/* The room's light on average, for scenery with no lit scenery near it. */
	uint64_t was[3] = { 1, 1, 1 }, now[3] = { 1, 1, 1 };
	for (unsigned y = 0; hd && scale >= 1 && y < height; y += 4)
		for (int x = 0; x < WIDTH; x += 4) {
			if (frame.id[y * WIDTH + x])
				continue;
			uint32_t a = pixel_at(console, pitch, x, (int)y);
			uint32_t b = hd[(size_t)(y * scale + scale / 2) * WIDTH * scale + x * scale + scale / 2];
			for (int c = 0; c < 3; c++)
				was[c] += a >> (8 * c) & 255, now[c] += b >> (8 * c) & 255;
		}
	uint32_t room_was = 0, room_lit = 0;
	for (int c = 0; c < 3; c++) {
		uint64_t ratio = now[c] * 128 / was[c];
		room_was |= 128u << (8 * c);
		room_lit |= (uint32_t)(ratio > 255 ? 255 : ratio) << (8 * c);
	}

	for (unsigned y = 0; y < height; y++) {
		int row = (int)y + m->top;
		int layers[4] = { 0 };
		for (int x = 0; x < WIDTH; x++) {
			uint8_t id = frame.id[y * WIDTH + x];
			uint32_t *under = &frame.under[y * WIDTH + x];
			if (id == DIORAMA_FLAT)
				*under = 0xff000000u | lit(ground->scenery[row][x], console, pitch, hd, scale, x, (int)y, room_was, room_lit);
			else if (id)
				*under = 0xff000000u | lit(ground->color[row][x], console, pitch, hd, scale, x, (int)y, room_was, room_lit);
			else
				*under = 0;
			if (!id && m->source[row][x] < 4)
				layers[m->source[row][x]]++;
		}
		int layer = 0;
		for (int i = 1; i < 4; i++)
			if (layers[i] > layers[layer])
				layer = i;
		frame.shift[y][0] = frame.shift[y][1] = 0;
		if (follows && layers[layer] && m->mode[row] == before.mode[row] && m->mode[row] != 7) {
			int dx = wrap(before.hoffset[row][layer] - m->hoffset[row][layer], 1024);
			int dy = wrap(before.voffset[row][layer] - m->voffset[row][layer], 1024);
			if (abs(dx) <= MAX_STEP && abs(dy) <= MAX_STEP)
				frame.shift[y][0] = (float)dx, frame.shift[y][1] = (float)dy;
		}
	}

	before.ok = true;
	before.number = m->frame;
	memcpy(before.mode, m->mode, sizeof before.mode);
	memcpy(before.hoffset, m->hoffset, sizeof before.hoffset);
	memcpy(before.voffset, m->voffset, sizeof before.voffset);
	memcpy(before.objects, m->objects, sizeof before.objects);
	return &frame;
}
