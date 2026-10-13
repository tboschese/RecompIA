#include "explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdpack.h"
#include "input.h"

static char dir[1024];
static bool on;
/* A run that presses nothing: left alone, a game plays its own demo (a fighting game's other
 * stages, an RPG's attract scenes), which random input would cut short. */
static bool watch;
static uint64_t rng;

/* Every key found, by the runs before (known) and this one. */
static uint64_t *seen;
static size_t seen_mask, seen_used;

static bool seen_add(uint64_t key)
{
	if (!seen || (seen_used + 1) * 2 > seen_mask + 1) {
		size_t size = seen ? (seen_mask + 1) * 2 : 1 << 14;
		uint64_t *bigger = calloc(size, sizeof *bigger);
		for (size_t i = 0; seen && i <= seen_mask; i++)
			if (seen[i]) {
				size_t j = seen[i] & (size - 1);
				while (bigger[j])
					j = (j + 1) & (size - 1);
				bigger[j] = seen[i];
			}
		free(seen);
		seen = bigger;
		seen_mask = size - 1;
	}
	size_t i = key & seen_mask;
	while (seen[i] && seen[i] != key)
		i = (i + 1) & seen_mask;
	if (seen[i])
		return false;
	seen[i] = key;
	seen_used++;
	return true;
}

/* New tiles since the last state kept. */
#define PENDING_MAX 4096
typedef struct { uint64_t key, shape; int16_t x, y; uint8_t layer; } NewTile;
static NewTile pending[PENDING_MAX];
static int pending_count, pending_shapes;
static long frames, pending_since, last_kept = -1000;

static uint64_t next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

void explore_init(const char *out, const char *known, unsigned seed)
{
	snprintf(dir, sizeof dir, "%s", out);
	on = true;
	watch = seed % 4 == 0;
	rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)seed * 0xbf58476d1ce4e5b9ULL);
	if (!rng)
		rng = 1;
	FILE *f = known ? fopen(known, "r") : NULL;
	char line[64];
	unsigned long long key;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "%llx", &key) == 1 && key)
			seen_add(key);
	if (f)
		fclose(f);
}

bool explore_on(void)
{
	return on;
}

/* Input in spans of a few frames to a second: mostly walking one way, the buttons tapped or
 * held, Start now and then. No game knowledge: menus, fights and fields all get pressed. */
unsigned explore_buttons(long frame)
{
	static long span_end;
	static unsigned held, tapped;
	(void)frame;
	/* A special move is a motion then a button (a quarter circle, back then forward, down
	 * twice), a few frames each. Random spans almost never draw one, and a spell, a fireball
	 * or a burst only shows when one is drawn. Forward is either side. */
	static unsigned motion[8];
	static int motion_len, motion_at;
	if (watch)
		return 0;
	if (motion_at < motion_len) {
		unsigned b = motion[motion_at / 4 < 8 ? motion_at / 4 : 7];
		motion_at++;
		return b;
	}
	if (frames >= span_end && next() % 100 < 15) {
		unsigned fw = 1u << (next() & 1 ? BTN_RIGHT : BTN_LEFT);
		unsigned bk = fw == 1u << BTN_RIGHT ? 1u << BTN_LEFT : 1u << BTN_RIGHT;
		unsigned dn = 1u << BTN_DOWN, up = 1u << BTN_UP;
		static const int actions[4] = { BTN_A, BTN_B, BTN_X, BTN_Y };
		unsigned hit = 1u << actions[next() % 4];
		const unsigned shapes[7][4] = {
			{ dn, dn | fw, fw, hit }, { dn, dn | bk, bk, hit }, { bk, fw, hit, 0 }, { fw, fw, hit, 0 },
			{ bk, bk, hit, 0 }, { dn, dn, hit, 0 }, { dn, up, hit, 0 },
		};
		const unsigned *s = shapes[next() % 7];
		motion_len = 0;
		for (int i = 0; i < 4 && s[i]; i++) {
			motion[i * 2] = s[i];
			motion[i * 2 + 1] = i + 1 < 4 && s[i + 1] ? 0 : s[i];
			motion_len = (i * 2 + 2) * 4;
		}
		motion_at = 0;
		span_end = frames + motion_len;
		return explore_buttons(frame);
	}
	if (frames >= span_end) {
		span_end = frames + 6 + (long)(next() % 50);
		held = tapped = 0;
		unsigned roll = (unsigned)(next() % 100);
		static const int arrows[4] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
		if (roll < 65)
			held |= 1u << arrows[next() % 4];
		if (roll < 15)
			held |= 1u << arrows[next() % 2 + 2] | 1u << arrows[next() % 2];
		static const int actions[6] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R };
		static const int odds[6] = { 35, 25, 15, 15, 8, 8 };
		for (int i = 0; i < 6; i++)
			if ((int)(next() % 100) < odds[i])
				*(next() & 1 ? &held : &tapped) |= 1u << actions[i];
		if (next() % 100 < 3)
			tapped |= 1u << BTN_START;
		if (next() % 200 < 1)
			tapped |= 1u << BTN_SELECT;
	}
	return held | (frames % 8 < 3 ? tapped : 0);
}

bool explore_frame(const CoreMotion *m, unsigned height)
{
	frames++;
	if (!m || !m->valid)
		return false;
	hdpack_keys_begin();
	for (int y = 0; y < (int)height; y++) {
		int row = y + m->top;
		if (m->brightness[row] < 15 || m->mode[row] == 0xff)
			continue;
		const uint16_t *tiles = m->tile[row];
		const uint8_t *palettes = m->palette[row], *formats = m->format[row];
		for (int x = 0; x < 256;) {
			int end = x + 1;
			while (end < 256 && tiles[end] == tiles[x] && palettes[end] == palettes[x] && formats[end] == formats[x])
				end++;
			if (formats[x]) {
				uint64_t key = hdpack_key(m, row, x);
				if (key && pending_count < PENDING_MAX && seen_add(key)) {
					uint64_t shape = hdpack_shape(m, row, x);
					if (seen_add(shape)) {
						if (pending_shapes++ == 0)
							pending_since = frames;
					} else {
						shape = 0;
					}
					if (pending_count == 0 && !shape)
						pending_since = frames;
					pending[pending_count++] = (NewTile){ key, shape, (int16_t)x, (int16_t)y,
						(uint8_t)(m->source[row][x] >= 4 ? 4 : m->source[row][x]) };
				}
			}
			x = end;
		}
	}
	/* A burst of new graphics settles over a few frames (a scene fading in, a menu opening):
	 * keep the frame once it has, or once a few are waiting too long. Known graphics in a new
	 * palette (a fade, a flash) are listed too, but wait for a frame kept for something else. */
	if (pending_count == 0 || frames - last_kept < 20)
		return false;
	if (pending_shapes == 0)
		return frames - pending_since >= 600;
	return pending_shapes >= 24 ? frames - pending_since >= 10 : frames - pending_since >= 90;
}

void explore_keep(const char *name)
{
	char path[1100];
	snprintf(path, sizeof path, "%s/keys.tsv", dir);
	FILE *f = fopen(path, "a");
	for (int i = 0; f && i < pending_count; i++)
		fprintf(f, "%016llx\t%s\t%d\t%d\t%u\t%016llx\n", (unsigned long long)pending[i].key, name, pending[i].x,
			pending[i].y, pending[i].layer, (unsigned long long)pending[i].shape);
	if (f)
		fclose(f);
	pending_count = pending_shapes = 0;
	last_kept = frames;
}

bool explore_pending(void)
{
	return pending_count > 0;
}
