#include "remaster.h"
#include "hdpack.h"
#include "video.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#define WIDTH 256
#define MAX_HEIGHT 240

static uint32_t image[MAX_HEIGHT * REMASTER_SCALE_MAX * WIDTH * REMASTER_SCALE_MAX];
static uint8_t shadowed[MAX_HEIGHT * REMASTER_SCALE_MAX * WIDTH * REMASTER_SCALE_MAX];
static int scale = REMASTER_SCALE;
#define SCALE scale

#define BANDS 32
typedef struct {
	RowsFn *rows;
	void *context;
	int height;
} Bands;

static void band(void *bands, size_t i)
{
	const Bands *b = bands;
	int y0 = (int)((size_t)b->height * i / BANDS), y1 = (int)((size_t)b->height * (i + 1) / BANDS);
	if (y0 < y1)
		b->rows(b->context, y0, y1);
}

void remaster_bands(int height, RowsFn *rows, void *context)
{
	Bands b = { rows, context, height };
#ifdef __APPLE__
	dispatch_apply_f(BANDS, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &b, band);
#else
	for (size_t i = 0; i < BANDS; i++)
		band(&b, i);
#endif
}

/* Flat step, same hue: 100 leaves the color, above 100 lightens, below darkens.
 * Multiplying keeps a dark boot dark-brown instead of clipping it to black. */
/* A flat step of the same tone: every channel by one factor. A lighter step stops where the
 * brightest channel reaches 255, so a cream cloud stays cream instead of turning white. */
static uint32_t shade(uint32_t color, int percent)
{
	int top = (int)(color >> 16 & 255);
	if ((int)(color >> 8 & 255) > top)
		top = (int)(color >> 8 & 255);
	if ((int)(color & 255) > top)
		top = (int)(color & 255);
	if (top * percent > 25500)
		percent = 25500 / top;
	int channel[3];
	channel[0] = (int)(color >> 16 & 255) * percent / 100;
	channel[1] = (int)(color >> 8 & 255) * percent / 100;
	channel[2] = (int)(color & 255) * percent / 100;
	uint32_t out = 0;
	for (int i = 0; i < 3; i++) {
		int c = channel[i];
		if (c < 0)
			c = 0;
		else if (c > 255)
			c = 255;
		out = out << 8 | (uint32_t)c;
	}
	return out;
}

/* Sprites share one key space so a character made of several objects is one silhouette.
 * Background layers keep their own keys, for the depth shade and the contact shadow. */
static int key_at(const CoreMotion *m, int row, int x)
{
	if (row < 0 || row >= MAX_HEIGHT || x < 0 || x >= WIDTH)
		return -1;
	uint8_t source = m->source[row][x];
	if (source >= 4 && source <= 5)
		return 0x100 + m->object[row][x];
	return source;
}

static bool sprite_key(int key)
{
	return key >= 0x100;
}

static uint32_t frame_at(const uint8_t *frame, size_t pitch, int x, int y)
{
	const uint32_t *row = (const uint32_t *)(frame + (size_t)y * pitch);
	return row[x] & 0xffffffu;
}

static const uint8_t *src_frame;
static size_t src_pitch;
static const CoreMotion *src_motion;
static unsigned src_height;

/* The color the game drew. A far layer used to be darkened for the whole
 * scanline whenever any nearer pixel shared that line, which laid a straight
 * shadow across the screen and over the ship. */
static uint32_t at_xy(int x, int y)
{
	if (x < 0 || y < 0 || x >= WIDTH || y >= (int)src_height)
		return 0;
	return frame_at(src_frame, src_pitch, x, y);
}


/* The picture as the game drew it, one console pixel per cell. */
static uint32_t grid[MAX_HEIGHT][WIDTH];
/* Height of the connected sprite. A letter is a short shape; a character is tall,
 * so a one-pixel hair column still belongs to the character. */
static uint8_t sprite_body[MAX_HEIGHT][WIDTH];

static bool sprite_pixel(int x, int y);

/* Where each character stands: the span of its lower quarter and its lowest row. */
typedef struct {
	int x0, x1, y;
} Feet;
#define FEET_MAX 32
static Feet feet[FEET_MAX];
static int feet_count;

static void prepare_grid(void)
{
	int height = (int)src_height;
	feet_count = 0;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < WIDTH; x++)
			grid[y][x] = at_xy(x, y);
	/* One flood per sprite, then every pixel of it learns the shape's height. */
	memset(sprite_body, 0, sizeof sprite_body);
	static int sprite_stack[WIDTH * MAX_HEIGHT];
	static int sprite_marks[WIDTH * MAX_HEIGHT];
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (!sprite_pixel(x, y) || sprite_body[y][x])
				continue;
			int count = 0;
			int top = 0;
			int min_y = y;
			int max_y = y;
			sprite_stack[top++] = y * WIDTH + x;
			sprite_body[y][x] = 1;
			while (top > 0) {
				int pos = sprite_stack[--top];
				sprite_marks[count++] = pos;
				int cy = pos / WIDTH;
				int cx = pos - cy * WIDTH;
				if (cy < min_y)
					min_y = cy;
				if (cy > max_y)
					max_y = cy;
				static const int near_y[4] = { 1, -1, 0, 0 };
				static const int near_x[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					int ny = cy + near_y[k];
					int nx = cx + near_x[k];
					if (ny < 0 || nx < 0 || ny >= height || nx >= WIDTH)
						continue;
					if (!sprite_pixel(nx, ny) || sprite_body[ny][nx])
						continue;
					sprite_body[ny][nx] = 1;
					sprite_stack[top++] = ny * WIDTH + nx;
				}
			}
			int body = max_y - min_y + 1;
			if (body > 255)
				body = 255;
			int fx0 = WIDTH, fx1 = -1, feet_from = max_y - body / 4;
			for (int i = 0; i < count; i++) {
				int pos = sprite_marks[i];
				int cy = pos / WIDTH, cx = pos - cy * WIDTH;
				sprite_body[cy][cx] = (uint8_t)body;
				if (cy >= feet_from) {
					fx0 = cx < fx0 ? cx : fx0;
					fx1 = cx > fx1 ? cx : fx1;
				}
			}
			/* a shape cut by the edge of the screen, or wider than a person, is scenery or a vehicle */
			if (body >= 24 && fx0 > 0 && fx1 < WIDTH - 1 && fx1 - fx0 < 48 && feet_count < FEET_MAX)
				feet[feet_count++] = (Feet){ fx0, fx1, max_y };
		}
	}
}

static bool sprite_pixel(int x, int y)
{
	if (x < 0 || y < 0 || x >= WIDTH || y >= (int)src_height)
		return false;
	return sprite_key(key_at(src_motion, y + src_motion->top, x));
}

static int luma_of(uint32_t color)
{
	return (int)((color >> 16 & 255) * 3 + (color >> 8 & 255) * 6 + (color & 255)) / 10;
}

typedef struct {
	unsigned out_w, out_h;
	int height;
} Canvas;


/* Dynamic light. The game already marks what glows: a flame drawn in the
 * scenery, or a spell the console adds on top of the picture with color math.
 * Each glowing blob becomes a light of its own color. It brightens what is
 * near it in flat rings, lights the edge of a figure that faces it, and the
 * figure throws a hard shadow away from it. A flame flickers. Every console
 * pixel gets one gain, so the art stays hard pixels and black stays black. */
#define LIGHT_MAX 8
/* A torch's drawing flickers, so a flame in the scenery not found in a frame holds for a while.
 * A burst or a fire on a figure that is gone fades out over these frames, and so does a torch
 * at the end of its hold. */
#define GLOW_HOLD 8
#define LIGHT_KEYS 32
typedef struct {
	int x, y, radius, area;
	int r, g, b;
	bool flame, pale, listed, wide;
	bool moving;  /* drawn by a sprite: a fire on a figure, a spell; when it is gone it is gone */
	int sources;  /* lights merged into this one; it shines as all of them */
	int life;
	/* for review: the tiles it is made of, the dimmest glowing pixel of each, its extent */
	int keys, x0, y0, x1, y1;
	uint64_t key[LIGHT_KEYS];
	uint8_t key_luma[LIGHT_KEYS];
} Light;

/* The game's own list of lights, by tile (lights.txt). */
typedef struct {
	uint64_t key;
	char kind;  /* 'f' flame, 'g' glow, 'w' window, 'n' none, 'a' water */
	uint8_t luma;
	int16_t r, g, b, radius;  /* -1: from what is drawn */
} Listed;
static Listed *listed;
static size_t listed_mask;
static int listed_count;
static bool detect_lights = true, finding_lights;
static LightFound found[LIGHT_MAX * LIGHT_KEYS];
static int found_count;
static uint64_t tile_keys[MAX_HEIGHT][WIDTH];
static int16_t listed_at[MAX_HEIGHT][WIDTH];
static uint8_t water[MAX_HEIGHT][WIDTH];
static int blob_px[MAX_HEIGHT * WIDTH];

static int listed_find(uint64_t key)
{
	if (!listed || !key)
		return -1;
	for (size_t i = key & listed_mask;; i = (i + 1) & listed_mask) {
		if (listed[i].key == key)
			return (int)i;
		if (!listed[i].key)
			return -1;
	}
}

bool remaster_load_lights(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char line[256];
	int lines = 0;
	while (fgets(line, sizeof line, f))
		lines++;
	size_t size = 64;
	while (size < (size_t)lines * 2)
		size *= 2;
	free(listed);
	listed = calloc(size, sizeof *listed);
	listed_mask = size - 1;
	listed_count = 0;
	rewind(f);
	while (listed && fgets(line, sizeof line, f)) {
		unsigned long long key;
		char kind[16], r[8] = "-", g[8] = "-", b[8] = "-", radius[8] = "-";
		int luma = 0;
		if (line[0] == '#' || sscanf(line, "%llx %15s %d %7s %7s %7s %7s", &key, kind, &luma, r, g, b, radius) < 2 || !key)
			continue;
		char k = !strcmp(kind, "flame") ? 'f' : !strcmp(kind, "glow") ? 'g' : !strcmp(kind, "window") ? 'w'
			: !strcmp(kind, "none") ? 'n' : !strcmp(kind, "water") ? 'a' : 0;
		if (!k)
			continue; /* ? : not reviewed yet */
		size_t i = key & listed_mask;
		while (listed[i].key && listed[i].key != key)
			i = (i + 1) & listed_mask;
		listed_count += !listed[i].key;
		listed[i] = (Listed){ key, k, (uint8_t)(luma < 0 ? 0 : luma > 255 ? 255 : luma),
			(int16_t)(r[0] == '-' ? -1 : atoi(r)), (int16_t)(g[0] == '-' ? -1 : atoi(g)),
			(int16_t)(b[0] == '-' ? -1 : atoi(b)), (int16_t)(radius[0] == '-' ? -1 : atoi(radius)) };
	}
	fclose(f);
	return listed_count > 0;
}

void remaster_detect_lights(bool on)
{
	detect_lights = on;
}

void remaster_find_lights(bool on)
{
	finding_lights = on;
	found_count = 0;
}

const LightFound *remaster_lights_found(int *count)
{
	*count = found_count;
	return found;
}
static uint8_t emits[MAX_HEIGHT][WIDTH];
static uint8_t by_light[MAX_HEIGHT][WIDTH]; /* within two pixels of a glowing pixel */
static int16_t gain[MAX_HEIGHT][WIDTH][3];
static int16_t glow[MAX_HEIGHT][WIDTH][3];
#define SHADOW_ANGLES 720
/* A figure's shadow lies on the ground near it; past this it would sweep a far backdrop. */
#define SHADOW_LENGTH 20.0f
static float shadow_near[SHADOW_ANGLES];

static int shadow_bin(float dx, float dy)
{
	float a = __builtin_atan2f(dy, dx);
	int bin = (int)((a + 3.14159265f) * (float)SHADOW_ANGLES / 6.2831853f);
	if (bin < 0)
		bin = 0;
	if (bin >= SHADOW_ANGLES)
		bin = SHADOW_ANGLES - 1;
	return bin;
}
static int light_stack[MAX_HEIGHT * WIDTH];
static uint8_t receives[MAX_HEIGHT][WIDTH];

/* Direction and distance of every offset a light can reach, worked out once. */
#define REACH 96
static int16_t reach_bin[2 * REACH + 1][2 * REACH + 1];
static float reach_d[2 * REACH + 1][2 * REACH + 1];

static void reach_init(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	for (int dy = -REACH; dy <= REACH; dy++)
		for (int dx = -REACH; dx <= REACH; dx++) {
			reach_bin[dy + REACH][dx + REACH] = (int16_t)shadow_bin((float)dx, (float)dy);
			reach_d[dy + REACH][dx + REACH] = __builtin_sqrtf((float)(dx * dx + dy * dy));
		}
}

/* Mode 1's third layer holds the text windows of most games. A window is not
 * in the room, so it neither glows nor takes light. */
static bool light_window(const CoreMotion *m, int row, int x)
{
	return m->source[row][x] == 2 && m->mode[row] == 1;
}

/* A text window is often the same layer as the room, drawn with its own
 * scroll on its own lines. The room is the scroll a layer has on most of the
 * lines where it shows; a pixel on another scroll is a window, not the room.
 * A layer that shows only in a band of a third of the screen is a window too. */
static uint16_t room_h[4], room_v[4];
static bool room_band[4];

static void find_room(const CoreMotion *m, int h)
{
	for (int layer = 0; layer < 4; layer++) {
		uint32_t keys[MAX_HEIGHT];
		int counts[MAX_HEIGHT], first[MAX_HEIGHT], last[MAX_HEIGHT];
		int distinct = 0;
		for (int y = 0; y < h; y++) {
			int row = y + m->top;
			int shown = 0;
			for (int x = 0; x < WIDTH; x += 8)
				shown += m->source[row][x] == layer;
			if (shown == 0)
				continue;
			uint32_t key = (uint32_t)m->hoffset[row][layer] << 16 | m->voffset[row][layer];
			int k = 0;
			while (k < distinct && keys[k] != key)
				k++;
			if (k == distinct) {
				keys[distinct] = key;
				first[distinct] = y;
				counts[distinct++] = 0;
			}
			counts[k] += shown;
			last[k] = y;
		}
		int best = 0;
		for (int k = 1; k < distinct; k++)
			if (counts[k] > counts[best])
				best = k;
		room_h[layer] = distinct ? (uint16_t)(keys[best] >> 16) : 0;
		room_v[layer] = distinct ? (uint16_t)(keys[best] & 0xffff) : 0;
		room_band[layer] = distinct && (last[best] - first[best] + 1) * 3 <= h;
	}
}

static bool light_plane(const CoreMotion *m, int row, int x)
{
	int layer = m->source[row][x];
	if (layer > 3)
		return true;
	return !room_band[layer] && m->hoffset[row][layer] == room_h[layer] && m->voffset[row][layer] == room_v[layer];
}

/* A tile drawn in one dark color is empty space around the room, not a
 * surface. Light does not land on it. Read from the tile data in VRAM. */
static bool light_void(const CoreMotion *m, int row, int x, uint32_t color)
{
	if (luma_of(color) >= 24)
		return false;
	int words = m->format[row][x] == 1 ? 8 : m->format[row][x] == 2 ? 16 : m->format[row][x] == 3 ? 32 : 0;
	if (words == 0)
		return m->source[row][x] == 6;
	unsigned base = m->tile[row][x];
	for (int plane = 0; plane < words / 8; plane++) {
		for (int half = 0; half < 2; half++) {
			int first = -1;
			for (int r = 0; r < 8; r++) {
				uint16_t w = m->vram[(base + (unsigned)plane * 8 + (unsigned)r) & 0x7fff];
				int byte = half ? w >> 8 : w & 0xff;
				if (byte != 0 && byte != 0xff)
					return false;
				if (first < 0)
					first = byte;
				else if (byte != first)
					return false;
			}
		}
	}
	return true;
}

static int emitter(const CoreMotion *m, int y, int x)
{
	int row = y + m->top;
	if (m->brightness[row] < 15)
		return 0;
	uint32_t f = at_xy(x, y);
	uint32_t own = m->color[row][x] & 0xffffff;
	int lf = luma_of(f);
	int r = f >> 16 & 255, g = f >> 8 & 255, b = f & 255;
	int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	bool sprite = sprite_key(key_at(m, row, x));
	int or_ = own >> 16 & 255, og = own >> 8 & 255, ob = own & 255;
	int ohi = or_ > og ? (or_ > ob ? or_ : ob) : (og > ob ? og : ob);
	int olo = or_ < og ? (or_ < ob ? or_ : ob) : (og < ob ? og : ob);
	/* A spell the console lays over the picture in see-through color: its
	 * own color is vivid and the picture under it adds on top. A window is
	 * drawn solid. */
	bool see_through = !sprite && m->source[row][x] < 4 && r >= or_ && g >= og && b >= ob
		&& lf - luma_of(own) >= 12 && ohi >= 200 && ohi - olo >= 120 && lf >= 110;
	if (see_through)
		return 8;
	if (light_window(m, row, x) || !light_plane(m, row, x))
		return 0;
	if (!sprite && r >= 232 && g >= 120 && g <= 200 && b <= 110 && r - b >= 150)
		return 2; /* a flame drawn in the scenery */
	if (!sprite && r >= 240 && g >= 200 && b <= 180 && r - b >= 60)
		return 4; /* the flame's hot core, a light only beside orange */
	if (sprite && lo >= 235 && hi - lo < 24)
		return 3; /* a white flash, such as a spell's burst */
	if (sprite && r >= 240 && g >= 220 && (b <= 140 || (g >= 240 && b <= 210)))
		return 7; /* the white-yellow heart of a fire the game draws as a sprite */
	if (sprite && r >= 230 && g >= 60 && g <= 200 && b <= 110 && r - b >= 150)
		return 6; /* the orange of that fire */
	if (lf >= 200 && hi - lo >= 60 && lf - luma_of(own) >= 48)
		return 1; /* a colored spell the console added over the picture */
	if (!sprite && lo >= 200 && lf >= 235)
		return 5; /* a pale bright patch: a window, a lamp, only if the wall around is darker */
	if (finding_lights && sprite && lf >= 150 && hi >= 220 && hi - lo >= 70)
		return 10; /* a vivid bright figure: a spell drawn as a plain sprite, or a costume.
		            * Only offered for review; it lights the game once the list names it. */
	return 0;
}

#define WIDE_LIGHT 48

static bool wide_light(const Light *l)
{
	return l->wide;
}

/* A figure standing in a band of lava cuts it into pieces. A piece of the color of a wide
 * light, at its height, is that band too, not a lamp of its own. */
static void mark_wide(Light *lights, int count)
{
	for (int k = 0; k < count; k++)
		lights[k].wide = lights[k].x1 - lights[k].x0 + 1 > WIDE_LIGHT || lights[k].y1 - lights[k].y0 + 1 > WIDE_LIGHT;
	for (int k = 0; k < count; k++) {
		Light *p = &lights[k];
		for (int j = 0; j < count && !p->wide; j++) {
			const Light *w = &lights[j];
			if (j == k || (w->x1 - w->x0 + 1 <= WIDE_LIGHT && w->y1 - w->y0 + 1 <= WIDE_LIGHT))
				continue;
			int overlap = (p->y1 < w->y1 ? p->y1 : w->y1) - (p->y0 > w->y0 ? p->y0 : w->y0) + 1;
			if (abs(p->r - w->r) <= 32 && abs(p->g - w->g) <= 32 && abs(p->b - w->b) <= 32
				&& overlap * 2 >= p->y1 - p->y0 + 1)
				p->wide = true;
		}
	}
}

static unsigned light_hash(unsigned v)
{
	v ^= v >> 16;
	v *= 0x7feb352dU;
	v ^= v >> 15;
	v *= 0x846ca68bU;
	v ^= v >> 16;
	return v;
}

static void apply_rows(void *context, int y0, int y1);
static void settle_rows(void *context, int y0, int y1);
static void find_bloom(int h);
typedef struct {
	const Light *light;
	int h;
} Shaft;
static void shaft_rows(void *context, int y0, int y1);
/* lights side by side (a lava pool found as several) share one ceiling, not one each */
#define SHAFT_MOST 48
static int16_t shafted[MAX_HEIGHT][WIDTH][3];
static void wide_pass(const CoreMotion *m, const Light *lights, int count, int h);

static void emit_rows(void *context, int y0, int y1)
{
	const CoreMotion *m = context;
	for (int y = y0; y < y1; y++)
		for (int x = 0; x < WIDTH; x++)
			emits[y][x] = (uint8_t)emitter(m, y, x);
}

/* Last frame's lights, for the key light of the figures' volume, drawn before this frame's. */
typedef struct {
	float x, y, reach;
	int warmth;  /* 0..256: how much redder than blue the light is */
} KeyLight;
static KeyLight key_lights[LIGHT_MAX];
static int key_count;

/* The game's own list, by tile: a run of one tile at a time. Before the picture is graded, so
 * the water is known to every pass. */
static bool listed_scanned;

static void find_water(int h);

static void scan_listed(const CoreMotion *m, int h)
{
	static FILE *key_dump;
	static bool key_dump_tried;
	if (!key_dump_tried) {
		key_dump_tried = true;
		if (getenv("REMASTERIA_KEY_DUMP"))
			key_dump = fopen(getenv("REMASTERIA_KEY_DUMP"), "wb");
	}
	memset(water, 0, sizeof water);
	listed_scanned = listed_count > 0 || finding_lights || key_dump;
	if (!listed_scanned)
		return;
	hdpack_keys_begin();
	for (int y = 0; y < h; y++) {
		int row = y + m->top;
		const uint16_t *tiles = m->tile[row];
		const uint8_t *palettes = m->palette[row], *formats = m->format[row];
		for (int x = 0; x < WIDTH;) {
			int end = x + 1;
			while (end < WIDTH && tiles[end] == tiles[x] && palettes[end] == palettes[x] && formats[end] == formats[x])
				end++;
			uint64_t key = hdpack_key(m, row, x);
			int at = listed_find(key);
			for (; x < end; x++) {
				if (finding_lights || key_dump)
					tile_keys[y][x] = key;
				listed_at[y][x] = (int16_t)at;
				if (at >= 0 && listed[at].kind == 'a')
					water[y][x] = !sprite_pixel(x, y) && !light_window(m, row, x);
			}
		}
	}
	find_water(h);
	if (key_dump) {
		/* for the tools: each frame's tile keys, layers and colors, to name the water by tile */
		int32_t rows = h;
		fwrite(&rows, sizeof rows, 1, key_dump);
		for (int y = 0; y < h; y++)
			fwrite(tile_keys[y], sizeof(uint64_t), WIDTH, key_dump);
		for (int y = 0; y < h; y++)
			fwrite(m->source[y + m->top], 1, WIDTH, key_dump);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < WIDTH; x++) {
				uint32_t c = at_xy(x, y);
				fwrite(&c, sizeof c, 1, key_dump);
			}
		fflush(key_dump);
	}
}

static void light_pass(const CoreMotion *m, unsigned height, unsigned out_w)
{
	int h = (int)height;
	find_room(m, h);
	if (detect_lights)
		remaster_bands(h, emit_rows, (void *)m);
	else
		memset(emits, 0, sizeof emits);
	/* The game's own list overrides the detection. */
	if (listed_scanned)
		for (int y = 0; y < h; y++) {
			int row = y + m->top;
			for (int x = 0; x < WIDTH; x++) {
				int at = listed_at[y][x];
				if (at < 0)
					continue;
				emits[y][x] = listed[at].kind != 'n' && listed[at].kind != 'a' && m->brightness[row] >= 15
					&& luma_of(m->color[row][x] & 0xffffff) >= listed[at].luma ? 9 : 0;
			}
		}
	memset(by_light, 0, sizeof by_light);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++)
			if (emits[y][x])
				for (int yy = y < 2 ? 0 : y - 2; yy <= y + 2 && yy < h; yy++)
					for (int xx = x < 2 ? 0 : x - 2; xx <= x + 2 && xx < WIDTH; xx++)
						by_light[yy][xx] = 1;

	/* A scene full of white is daylight outside, where a cloud is not a lamp. */
	int pale_total = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++)
			pale_total += emits[y][x] == 5;
	bool indoors = pale_total <= 1200;

	Light lights[LIGHT_MAX];
	int count = 0, pale_blobs = 0;
	static uint8_t seen[MAX_HEIGHT][WIDTH];
	memset(seen, 0, sizeof seen);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (!emits[y][x] || seen[y][x])
				continue;
			int top = 0, area = 0, flames = 0, flashes = 0, orange = 0, lit_side = 0, pales = 0;
			int fire = 0, heart = 0, spells = 0, veil = 0, vivid = 0, named = 0, entry = -1, drawn_by_sprite = 0;
			int bx0 = x, bx1 = x, by0 = y, by1 = y;
			long rim_luma = 0, rim_count = 0, own_luma = 0, rim_bright = 0;
			long sx = 0, sy = 0, sr = 0, sg = 0, sb = 0;
			light_stack[top++] = y * WIDTH + x;
			seen[y][x] = 1;
			while (top > 0) {
				int pos = light_stack[--top];
				int cy = pos / WIDTH, cx = pos - cy * WIDTH;
				uint32_t c = at_xy(cx, cy);
				blob_px[area++] = pos;
				if (emits[cy][cx] == 9) {
					named++;
					entry = listed_at[cy][cx];
				}
				flames += emits[cy][cx] == 2 || emits[cy][cx] == 4;
				orange += emits[cy][cx] == 2;
				flashes += emits[cy][cx] == 3;
				spells += emits[cy][cx] == 1;
				veil += emits[cy][cx] == 8;
				vivid += emits[cy][cx] == 10;
				pales += emits[cy][cx] == 5;
				fire += emits[cy][cx] == 6;
				bx0 = cx < bx0 ? cx : bx0;
				bx1 = cx > bx1 ? cx : bx1;
				by0 = cy < by0 ? cy : by0;
				by1 = cy > by1 ? cy : by1;
				heart += emits[cy][cx] == 7;
				drawn_by_sprite += sprite_pixel(cx, cy);
				own_luma += luma_of(c);
				for (int side = -1; side <= 1; side += 2) {
					int xx = cx + side;
					if (xx >= 0 && xx < WIDTH && !emits[cy][xx] && luma_of(at_xy(xx, cy)) > 40)
						lit_side++;
				}
				sx += cx;
				sy += cy;
				sr += c >> 16 & 255;
				sg += c >> 8 & 255;
				sb += c & 255;
				static const int ny[4] = { 1, -1, 0, 0 };
				static const int nx[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					int yy = cy + ny[k], xx = cx + nx[k];
					if (yy < 0 || xx < 0 || yy >= h || xx >= WIDTH)
						continue;
					if (!emits[yy][xx]) {
						int rl = luma_of(at_xy(xx, yy));
						rim_luma += rl;
						rim_bright += rl >= 150;
						rim_count++;
					}
					if (seen[yy][xx] || !emits[yy][xx])
						continue;
					seen[yy][xx] = 1;
					light_stack[top++] = yy * WIDTH + xx;
				}
			}
			Light l;
			bool pale = false;
			const Listed *named_light = named * 2 > area ? &listed[entry] : NULL;
			if (named_light) {
				if (area > 6000)
					continue;
				l.flame = named_light->kind == 'f';
				pale = named_light->kind == 'w';
			} else {
				int contrast = rim_count ? (int)(own_luma / area - rim_luma / rim_count) : 0;
				bool burning = (fire + heart) * 2 > area && heart >= 2 && fire >= 4 && area >= 8
					&& rim_bright * 10 <= rim_count;
				if (burning) {
					/* Hair of the same colors sits on a figure of skin and cloth;
					 * a fire has only dark and red around it. */
					int cloth = 0;
					for (int yy = by0 - 6 < 0 ? 0 : by0 - 6; yy <= by1 + 6 && yy < h; yy++)
						for (int xx = bx0 - 6 < 0 ? 0 : bx0 - 6; xx <= bx1 + 6 && xx < WIDTH; xx++) {
							if (emits[yy][xx] || !sprite_pixel(xx, yy))
								continue;
							uint32_t c = at_xy(xx, yy);
							int cr = c >> 16 & 255, cg = c >> 8 & 255, cb = c & 255;
							if (luma_of(c) >= 100 && !(cr >= 2 * cg && cr >= 2 * cb))
								cloth++;
						}
					burning = cloth * 4 <= area;
				}
				bool glowing = flames * 2 > area && area >= 300 && contrast >= 50;
				bool spell_veil = veil * 2 > area && area >= 40
					&& area * 10 < (bx1 - bx0 + 1) * (by1 - by0 + 1) * 9; /* a solid box is a window */
				if (area < 2 || area > (glowing ? 2000 : spell_veil ? 6000 : 600))
					continue; /* a speck, or a whole sky the game tinted */
				l.flame = burning || (flames * 2 > area && !glowing);
				pale = !l.flame && pales * 2 > area;
				if (l.flame && !burning && (area > 40 || orange == 0 || (orange == flames && area > 4) || lit_side > 0))
					continue; /* a flame burns in dark air, orange around a hot core or a
					           * candle's tip; gold trim sits on a lit thing, a stripe of
					           * cloth is one flat color */
				if (pale) {
					int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
					pale_blobs += area >= 6;
					if (!indoors || area < 40 || area > 400 || rim_count == 0 || contrast < 50
						|| bw < 4 || bh < 4 || area * 5 < bw * bh * 3)
						continue; /* white paint, a cloud in daylight, a glint on metal, a letter */
				} else if (flashes * 2 > area && rim_bright * 4 > rim_count) {
					continue; /* a white figure on a bright ground, not a burst */
				} else if (vivid * 2 > area) {
					if (area < 12)
						continue; /* a glint */
				} else if (!l.flame && !glowing && !spell_veil && (area < 40 || (flashes + spells) * 2 <= area)) {
					continue; /* a highlight in the art, not a burst of light */
				}
			}
			l.x = (int)(sx / area);
			l.y = (int)(sy / area);
			l.area = l.flame ? area * 12 : area;
			int r = (int)(sr / area), g = (int)(sg / area), b = (int)(sb / area);
			int top_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
			if (top_c < 1)
				continue;
			l.r = r * 255 / top_c;
			l.g = g * 255 / top_c;
			l.b = b * 255 / top_c;
			l.r += (255 - l.r) / 4;
			l.g += (255 - l.g) / 4;
			l.b += (255 - l.b) / 4;
			if (named_light && named_light->r >= 0 && named_light->g >= 0 && named_light->b >= 0) {
				l.r = named_light->r;
				l.g = named_light->g;
				l.b = named_light->b;
			}
			int radius = l.flame ? 64 : 32;
			for (int s = 1; s * s <= area; s++)
				radius = (l.flame ? 64 : 32) + 3 * s;
			if (named_light && named_light->radius > 0)
				radius = named_light->radius;
			if (radius > REACH)
				radius = REACH;
			if (pale && (area >= 60 || named_light))
				l.y += radius / 3; /* daylight through a window falls into the room */
			l.radius = radius;
			l.pale = pale && !named_light;
			l.listed = named_light != NULL;
			l.moving = drawn_by_sprite * 2 > area;
			l.sources = 1;
			l.x0 = bx0;
			l.y0 = by0;
			l.x1 = bx1;
			l.y1 = by1;
			l.keys = 0;
			for (int i = 0; finding_lights && !named_light && i < area; i++) {
				int cy = blob_px[i] / WIDTH, cx = blob_px[i] % WIDTH;
				uint64_t key = tile_keys[cy][cx];
				int own = luma_of(m->color[cy + m->top][cx] & 0xffffff);
				int k = 0;
				while (k < l.keys && l.key[k] != key)
					k++;
				if (!key || k == LIGHT_KEYS)
					continue;
				if (k == l.keys) {
					l.key[l.keys] = key;
					l.key_luma[l.keys++] = (uint8_t)own;
				} else if (own < l.key_luma[k]) {
					l.key_luma[k] = (uint8_t)own;
				}
			}
			if (count < LIGHT_MAX) {
				lights[count++] = l;
			} else {
				int small = 0;
				for (int i = 1; i < count; i++)
					if (lights[i].area < lights[small].area)
						small = i;
				if (lights[small].area < area)
					lights[small] = l;
			}
		}
	}
	/* In a scene this bright it is daylight, and a small light adds nothing. */
	long scene_luma = 0;
	for (int y = 0; y < h; y += 4)
		for (int x = 0; x < WIDTH; x += 4)
			scene_luma += luma_of(at_xy(x, y));
	if (scene_luma * 16 >= 100L * h * WIDTH) {
		int kept_lights = 0;
		for (int i = 0; i < count; i++)
			if (lights[i].listed)
				lights[kept_lights++] = lights[i];
		count = kept_lights;
	}
	/* Many white patches are shiny art, not lamps. */
	if (pale_blobs > 8) {
		int kept_lights = 0;
		for (int i = 0; i < count; i++)
			if (!lights[i].pale)
				lights[kept_lights++] = lights[i];
		count = kept_lights;
	}
	found_count = 0;
	for (int i = 0; finding_lights && i < count; i++)
		for (int k = 0; k < lights[i].keys; k++)
			found[found_count++] = (LightFound){ lights[i].key[k], lights[i].flame ? 'f' : lights[i].pale ? 'w' : 'g',
				lights[i].key_luma[k], (uint8_t)lights[i].r, (uint8_t)lights[i].g, (uint8_t)lights[i].b,
				lights[i].radius, lights[i].x0, lights[i].y0, lights[i].x1, lights[i].y1 };
	/* The tongues of one listed fire are one light, as bright as all of them. (The pieces of a
	 * glow are not: they are one light the threshold cut apart.) */
	for (int i = 0; i < count; i++)
		for (int j = i + 1; j < count; j++) {
			Light *a = &lights[i], *b = &lights[j];
			int dx = a->x - b->x, dy = a->y - b->y;
			if (!a->listed || !b->listed || !a->flame || !b->flame || dx * dx + dy * dy >= 24 * 24)
				continue;
			int wa = a->area, wb = b->area, w = wa + wb;
			a->x = (a->x * wa + b->x * wb) / w;
			a->y = (a->y * wa + b->y * wb) / w;
			a->r = (a->r * wa + b->r * wb) / w;
			a->g = (a->g * wa + b->g * wb) / w;
			a->b = (a->b * wa + b->b * wb) / w;
			a->radius = a->radius > b->radius ? a->radius : b->radius;
			a->area = w;
			a->sources += b->sources;
			a->x0 = a->x0 < b->x0 ? a->x0 : b->x0;
			a->y0 = a->y0 < b->y0 ? a->y0 : b->y0;
			a->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
			a->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
			lights[j] = lights[--count];
			j = i; /* the merged light may now reach one passed over */
		}
	/* A flame's drawing changes every few frames and is not found in every
	 * one of them. A light that was seen stays for a moment, so the room does
	 * not blink. A light found again near where it was keeps its place. */
	static Light kept[LIGHT_MAX];
	static int kept_count;
	bool matched[LIGHT_MAX] = { false };
	for (int k = 0; k < kept_count; k++)
		kept[k].life--;
	for (int i = 0; i < count; i++) {
		int best = -1, best_d = 12 * 12;
		for (int k = 0; k < kept_count; k++) {
			int dx = kept[k].x - lights[i].x, dy = kept[k].y - lights[i].y;
			if (!matched[k] && dx * dx + dy * dy < best_d) {
				best_d = dx * dx + dy * dy;
				best = k;
			}
		}
		lights[i].life = lights[i].flame && !lights[i].moving ? 24 : GLOW_HOLD;
		if (best >= 0) {
			matched[best] = true;
			kept[best] = lights[i];
		} else if (kept_count < LIGHT_MAX) {
			matched[kept_count] = true;
			kept[kept_count++] = lights[i];
		}
	}
	int alive = 0;
	for (int k = 0; k < kept_count; k++)
		if (kept[k].life > 0)
			kept[alive++] = kept[k];
	kept_count = alive;
	/* A fire that moves is one light, not one more for every place it was seen. */
	for (int i = 0; i < kept_count; i++)
		for (int j = i + 1; j < kept_count; j++) {
			Light *a = &kept[i], *b = &kept[j];
			int dx = a->x - b->x, dy = a->y - b->y;
			if (a->flame != b->flame || a->pale != b->pale || dx * dx + dy * dy >= 24 * 24)
				continue;
			if (b->life > a->life || (b->life == a->life && b->area > a->area))
				*a = *b;
			kept[j] = kept[--kept_count];
			j = i;
		}
	count = kept_count;
	for (int k = 0; k < count; k++)
		lights[k] = kept[k];
	/* A wide light (a band of lava, a big window, a spell over the screen) is not a point: it
	 * lights what is near any part of it, by distance to its nearest glowing pixel, and casts no
	 * shadow. A point light (a torch, a lamp) lights in rings from its center, and a figure in
	 * front of it throws a shadow. */
	mark_wide(lights, count);
	key_count = 0;
	for (int k = 0; k < count && key_count < LIGHT_MAX; k++) {
		const Light *l = &lights[k];
		key_lights[key_count++] = (KeyLight){ (float)(l->x0 + l->x1) * 0.5f, (float)(l->y0 + l->y1) * 0.5f,
			(float)(wide_light(l) ? 96 : l->radius > 24 ? l->radius : 24),
			l->r - l->b <= 0 ? 0 : (l->r - l->b) * 2 > 256 ? 256 : (l->r - l->b) * 2 };
	}
	int points = 0;
	for (int k = 0; k < count; k++)
		points += !wide_light(&lights[k]);
	if (getenv("REMASTERIA_LIGHT_DEBUG"))
		for (int k = 0; k < count; k++)
			fprintf(stderr, "luz %d: %s %s caixa %d,%d-%d,%d cor %d %d %d raio %d\n", k, wide_light(&lights[k]) ? "larga" : "ponto",
				lights[k].flame ? "chama" : lights[k].pale ? "janela" : "brilho", lights[k].x0, lights[k].y0, lights[k].x1,
				lights[k].y1, lights[k].r, lights[k].g, lights[k].b, lights[k].radius);
	/* A room with a light in it falls darker around that light, and the eye
	 * adjusts over a few frames instead of the whole picture blinking. A wide light lights
	 * the room instead. */
	static int ambient = 256;
	/* A dark room falls darker around its light; a lit street does not. */
	int scene = (int)(scene_luma * 16 / ((long)h * WIDTH));
	int dim = scene <= 60 ? 80 : scene >= 90 ? 0 : 80 * (90 - scene) / 30;
	int target = points > 0 ? 256 - dim : 256;
	if (ambient > target)
		ambient -= ambient - target > 6 ? 6 : ambient - target;
	else if (ambient < target)
		ambient += target - ambient > 6 ? 6 : target - ambient;
	if (count == 0 && ambient == 256)
		return;

	reach_init();
	/* Empty space is a wide field of one flat dark color. A few flat tiles
	 * inside a dark sky are part of the sky and take its light. */
	static uint8_t flat[MAX_HEIGHT][WIDTH];
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++)
			flat[y][x] = light_void(m, y + m->top, x, at_xy(x, y));
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (flat[y][x] != 1)
				continue;
			uint32_t color = at_xy(x, y);
			int top = 0, area = 0, first = 0;
			light_stack[top++] = y * WIDTH + x;
			flat[y][x] = 2;
			while (first < top) {
				int pos = light_stack[first++];
				int cy = pos / WIDTH, cx = pos - cy * WIDTH;
				area++;
				static const int ny[4] = { 1, -1, 0, 0 };
				static const int nx[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					int yy = cy + ny[k], xx = cx + nx[k];
					if (yy < 0 || xx < 0 || yy >= h || xx >= WIDTH || flat[yy][xx] != 1 || at_xy(xx, yy) != color)
						continue;
					flat[yy][xx] = 2;
					light_stack[top++] = yy * WIDTH + xx;
				}
			}
			uint8_t mark = area >= 1500 ? 3 : 4;
			for (int i = 0; i < top; i++)
				flat[light_stack[i] / WIDTH][light_stack[i] % WIDTH] = mark;
		}
	}
	for (int y = 0; y < h; y++) {
		int row = y + m->top;
		for (int x = 0; x < WIDTH; x++) {
			bool room = light_plane(m, row, x);
			bool window = light_window(m, row, x);
			receives[y][x] = !emits[y][x] && room && !window && flat[y][x] != 3;
			int base = emits[y][x] || !room || window || m->brightness[row] < 15 ? 256 : ambient;
			gain[y][x][0] = gain[y][x][1] = gain[y][x][2] = (int16_t)base;
			glow[y][x][0] = glow[y][x][1] = glow[y][x][2] = 0;
		}
	}

	for (int i = 0; i < count; i++) {
		Light *l = &lights[i];
		if (wide_light(l))
			continue;
		int power = 100;
		if (l->flame)
			power = 82 + (int)(light_hash(m->frame / 3 * 131u + (unsigned)i * 977u) % 19);
		if (l->life < GLOW_HOLD)
			power = power * l->life / GLOW_HOLD;
		/* the smooth falloff is dim at its edge, so the pool reaches farther to light as much of the room */
		int radius = l->radius * (l->flame ? 94 + (int)(light_hash(m->frame / 5 + (unsigned)i) % 7) : 100) * 135 / 10000;
		if (radius > REACH)
			radius = REACH;
		int x0 = l->x - radius, x1 = l->x + radius, y0 = l->y - radius, y1 = l->y + radius;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 >= WIDTH)
			x1 = WIDTH - 1;
		if (y1 >= h)
			y1 = h - 1;

		/* A figure blocks the light. Per direction from the light, keep the
		 * nearest figure pixel; scenery farther along that direction is in its
		 * shadow. */
		for (int a = 0; a < SHADOW_ANGLES; a++)
			shadow_near[a] = 1e9f;
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				if (!sprite_pixel(x, y) || by_light[y][x] || sprite_body[y][x] < 14)
					continue;
				if (x >= l->x0 - 4 && x <= l->x1 + 4 && y >= l->y0 - 4 && y <= l->y1 + 4)
					continue; /* the figure the fire burns on does not shade its own fire */
				float d = reach_d[y - l->y + REACH][x - l->x + REACH];
				if (d < 1.0f || d >= (float)radius)
					continue;
				int bin = reach_bin[y - l->y + REACH][x - l->x + REACH];
				int spread = (int)(0.75f / d * (float)SHADOW_ANGLES / 6.2831853f) + 1;
				for (int s = -spread; s <= spread; s++) {
					int b = (bin + s + SHADOW_ANGLES) % SHADOW_ANGLES;
					if (d < shadow_near[b])
						shadow_near[b] = d;
				}
			}
		}

		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				if (!receives[y][x])
					continue;
				int dx = l->x - x, dy = l->y - y;
				if (dx * dx + dy * dy >= radius * radius)
					continue;
				float d = reach_d[dy + REACH][dx + REACH];
				float a = 1.0f - d / (float)radius;
				/* smoothstep: the pool fades out with no slope at its edge, so no rim shows */
				float ring = a * a * (3.0f - 2.0f * a) * 4.0f;
				if (ring <= 0.0f)
					continue;
				int k; /* in hundredths of the light's color */
				if (sprite_pixel(x, y)) {
					int tx = x, ty = y;
					if (d > 0.5f) {
						tx = x + (int)__builtin_lroundf(dx / d);
						ty = y + (int)__builtin_lroundf(dy / d);
					}
					int bx = 2 * x - tx, by = 2 * y - ty;
					bool facing = !sprite_pixel(tx, ty);
					bool away = !sprite_pixel(bx, by);
					if (sprite_body[y][x] < 14)
						k = ring * 14;
					else if (facing)
						k = 50 + ring * 26; /* the edge that faces the light */
					else if (away)
						k = 0;
					else
						k = ring * 16;
				} else if (d > shadow_near[reach_bin[REACH - dy][REACH - dx]] + 1.0f
					&& d < shadow_near[reach_bin[REACH - dy][REACH - dx]] + SHADOW_LENGTH) {
					k = 0; /* the figure stands between this pixel and the light */
				} else {
					k = ring * 30;
				}
				k = k * power / 100 * l->sources;
				if (k >= 0) {
					gain[y][x][0] += (int16_t)(k * l->r * 256 / 25500);
					gain[y][x][1] += (int16_t)(k * l->g * 256 / 25500);
					gain[y][x][2] += (int16_t)(k * l->b * 256 / 25500);
					/* Light falling on a dark floor: a flat step of the light's own
					 * color, so a black floor shows the pool too. */
					int add = k * power / 100 / 4;
					glow[y][x][0] += (int16_t)(add * l->r / 255);
					glow[y][x][1] += (int16_t)(add * l->g / 255);
					glow[y][x][2] += (int16_t)(add * l->b / 255);
				} else {
					for (int c = 0; c < 3; c++)
						gain[y][x][c] += (int16_t)(k * 256 / 100);
				}
			}
		}
	}
	if (points < count)
		wide_pass(m, lights, count, h);

	Canvas canvas = { out_w, (unsigned)h * SCALE, h };
	remaster_bands(h, settle_rows, NULL);
	find_bloom(h);
	memset(shafted, 0, sizeof shafted);
	for (int k = 0; k < count; k++)
		if (wide_light(&lights[k]) && lights[k].x1 - lights[k].x0 >= 6 && lights[k].y1 - lights[k].y0 >= 6) {
			Shaft s = { &lights[k], h };
			remaster_bands(h, shaft_rows, &s);
		}
	remaster_bands(h, apply_rows, &canvas);
}

/* Wide lights: from every glowing pixel of them at once, outward, each pixel learns the nearest
 * one and how far it is. Flat rings by that distance on the scenery; on a figure, the edge that
 * faces the glow takes its color, the way lava or a bright window rims what stands before it. */
#define WIDE_REACH 54
static void wide_pass(const CoreMotion *m, const Light *lights, int count, int h)
{
	static uint8_t owner[MAX_HEIGHT][WIDTH];
	static int16_t from_x[MAX_HEIGHT][WIDTH], from_y[MAX_HEIGHT][WIDTH];
	memset(owner, 0xff, sizeof owner);
	int head = 0, tail = 0;
	for (int i = 0; i < count; i++) {
		const Light *l = &lights[i];
		if (!wide_light(l))
			continue;
		for (int y = l->y0 < 0 ? 0 : l->y0; y <= l->y1 && y < h; y++)
			for (int x = l->x0 < 0 ? 0 : l->x0; x <= l->x1 && x < WIDTH; x++)
				if (emits[y][x] && owner[y][x] == 0xff) {
					owner[y][x] = (uint8_t)i;
					from_x[y][x] = (int16_t)x;
					from_y[y][x] = (int16_t)y;
					light_stack[tail++] = y * WIDTH + x;
				}
	}
	while (head < tail) {
		int pos = light_stack[head++];
		int cy = pos / WIDTH, cx = pos - cy * WIDTH;
		static const int ny[4] = { 1, -1, 0, 0 };
		static const int nx[4] = { 0, 0, 1, -1 };
		for (int k = 0; k < 4; k++) {
			int yy = cy + ny[k], xx = cx + nx[k];
			if (yy < 0 || xx < 0 || yy >= h || xx >= WIDTH || owner[yy][xx] != 0xff)
				continue;
			int dx = xx - from_x[cy][cx], dy = yy - from_y[cy][cx];
			if (dx * dx + dy * dy > WIDE_REACH * WIDE_REACH)
				continue;
			owner[yy][xx] = owner[cy][cx];
			from_x[yy][xx] = from_x[cy][cx];
			from_y[yy][xx] = from_y[cy][cx];
			light_stack[tail++] = yy * WIDTH + xx;
		}
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (owner[y][x] == 0xff || !receives[y][x])
				continue;
			const Light *l = &lights[owner[y][x]];
			bool inside = false;
			for (int i = 0; i < count && !inside; i++)
				inside = wide_light(&lights[i]) && x >= lights[i].x0 && x <= lights[i].x1 && y >= lights[i].y0 && y <= lights[i].y1;
			if (inside && !sprite_pixel(x, y))
				continue; /* the picture of the light itself (a stained glass, the crust on lava) stays as drawn */
			int dx = from_x[y][x] - x, dy = from_y[y][x] - y;
			float d = __builtin_sqrtf((float)(dx * dx + dy * dy));
			float wa = 1.0f - d / (float)WIDE_REACH;
			float ring = wa * wa * (3.0f - 2.0f * wa) * 4.0f;
			if (ring <= 0.0f)
				continue;
			int power = l->flame ? 90 + (int)(light_hash(m->frame / 6 * 131u + owner[y][x] * 977u) % 11) : 100;
			if (l->life < GLOW_HOLD)
				power = power * l->life / GLOW_HOLD;
			int k;
			if (sprite_pixel(x, y)) {
				int tx = x, ty = y;
				if (d > 0.5f) {
					tx = x + (int)__builtin_lroundf((float)dx / d);
					ty = y + (int)__builtin_lroundf((float)dy / d);
				}
				if (sprite_body[y][x] < 14)
					k = ring * 12;
				else if (!sprite_pixel(tx, ty))
					k = 30 + ring * 16; /* the edge that faces the glow */
				else
					k = ring * 8;
			} else {
				k = ring * 14;
			}
			k = k * power / 100;
			gain[y][x][0] += (int16_t)(k * l->r * 256 / 25500);
			gain[y][x][1] += (int16_t)(k * l->g * 256 / 25500);
			gain[y][x][2] += (int16_t)(k * l->b * 256 / 25500);
			int add = k / 4;
			glow[y][x][0] += (int16_t)(add * l->r / 255);
			glow[y][x][1] += (int16_t)(add * l->g / 255);
			glow[y][x][2] += (int16_t)(add * l->b / 255);
		}
	}
}

/* What each console pixel ends up with: a gain per channel (256 leaves it) and a color added on
 * top, and which surface it belongs to. On screen the light runs smoothly from one console pixel's
 * center to the next, but never across the edge of a figure or into a window. */
static int16_t lit[MAX_HEIGHT][WIDTH][6];
static uint8_t surface[MAX_HEIGHT][WIDTH];
static bool lit_any[MAX_HEIGHT][WIDTH];
/* The soft halo a bright fire, lava or spell throws past its own pixels. */
static int16_t bloom[MAX_HEIGHT][WIDTH][3];
#define BLOOM_REACH 6

static void settle_rows(void *context, int y0, int y1)
{
	(void)context;
	for (int y = y0; y < y1; y++) {
		for (int x = 0; x < WIDTH; x++) {
			surface[y][x] = (uint8_t)(sprite_pixel(x, y) ? 1 : receives[y][x] || emits[y][x] ? 0 : 2);
			int gr = gain[y][x][0], gg = gain[y][x][1], gb = gain[y][x][2];
			int ar = glow[y][x][0], ag = glow[y][x][1], ab = glow[y][x][2];
			int16_t *o = lit[y][x];
			lit_any[y][x] = !(gr == 256 && gg == 256 && gb == 256 && (ar | ag | ab) == 0);
			if (!lit_any[y][x]) {
				o[0] = o[1] = o[2] = 256;
				o[3] = o[4] = o[5] = 0;
				continue;
			}
			int g3[3] = { gr, gg, gb };
			for (int c = 0; c < 3; c++) {
				if (g3[c] < 140)
					g3[c] = 140;
				if (g3[c] > 700)
					g3[c] = 700;
			}
			/* A light brings a pale wall halfway to white, not into a flat white patch. */
			int most = g3[0] > g3[1] ? (g3[0] > g3[2] ? g3[0] : g3[2]) : (g3[1] > g3[2] ? g3[1] : g3[2]);
			int lum = luma_of(at_xy(x, y));
			int cap = lum > 0 ? (lum + (235 - lum) / 2) * 256 / lum : 700;
			if (cap < 256)
				cap = 256;
			if (most > cap)
				for (int c = 0; c < 3; c++)
					if (g3[c] > 256)
						g3[c] = 256 + (g3[c] - 256) * (cap - 256) / (most - 256);
			int room = lum < 235 ? (235 - lum) / 2 : 0;
			int add = ar > ag ? (ar > ab ? ar : ab) : (ag > ab ? ag : ab);
			if (add > room) {
				ar = ar * room / add;
				ag = ag * room / add;
				ab = ab * room / add;
			}
			o[0] = (int16_t)g3[0], o[1] = (int16_t)g3[1], o[2] = (int16_t)g3[2];
			o[3] = (int16_t)ar, o[4] = (int16_t)ag, o[5] = (int16_t)ab;
		}
	}
}

/* Shafts: a bright window or a lava pool lights the air in front of it. From each pixel near
 * it, the way back to its center is walked; every glowing pixel of it on that way adds to the
 * pixel, so the light leaves in rays, and a figure standing in the glow cuts a dark ray. */
#define SHAFT_REACH 72
#define SHAFT_STEPS 16

static void shaft_rows(void *context, int y0, int y1)
{
	const Shaft *s = context;
	const Light *l = s->light;
	float cx = (float)(l->x0 + l->x1) * 0.5f, cy = (float)(l->y0 + l->y1) * 0.5f;
	int ax = l->x0 - SHAFT_REACH, bx = l->x1 + SHAFT_REACH, ay = l->y0 - SHAFT_REACH, by = l->y1 + SHAFT_REACH;
	if (y0 < ay)
		y0 = ay;
	if (y1 > by + 1)
		y1 = by + 1;
	for (int y = y0; y < y1; y++)
		for (int x = ax < 0 ? 0 : ax; x <= bx && x < WIDTH; x++) {
			int ex = x < l->x0 ? l->x0 - x : x > l->x1 ? x - l->x1 : 0;
			int ey = y < l->y0 ? l->y0 - y : y > l->y1 ? y - l->y1 : 0;
			float far = __builtin_sqrtf((float)(ex * ex + ey * ey));
			if (far <= 0.0f || far >= (float)SHAFT_REACH || sprite_pixel(x, y))
				continue;
			float dx = (cx - (float)x) / SHAFT_STEPS, dy = (cy - (float)y) / SHAFT_STEPS;
			float px = (float)x, py = (float)y;
			int sum[3] = { 0, 0, 0 }, hits = 0;
			for (int i = 0; i < SHAFT_STEPS; i++, px += dx, py += dy) {
				int qx = (int)px, qy = (int)py;
				if (qx < l->x0 || qx > l->x1 || qy < l->y0 || qy > l->y1 || !emits[qy][qx])
					continue;
				uint32_t c = at_xy(qx, qy);
				if (luma_of(c) < 120)
					continue;
				sum[0] += (int)(c >> 16 & 255), sum[1] += (int)(c >> 8 & 255), sum[2] += (int)(c & 255);
				hits++;
			}
			if (!hits)
				continue;
			float fade = 1.0f - far / (float)SHAFT_REACH;
			fade *= fade;
			for (int c = 0; c < 3; c++) {
				int add = (int)((float)sum[c] / SHAFT_STEPS * fade * 0.5f);
				if (shafted[y][x][c] + add > SHAFT_MOST)
					add = SHAFT_MOST - shafted[y][x][c];
				if (add <= 0)
					continue;
				shafted[y][x][c] = (int16_t)(shafted[y][x][c] + add);
				int v = bloom[y][x][c] + add;
				bloom[y][x][c] = (int16_t)(v > 255 ? 255 : v);
			}
		}
}

static void find_bloom(int h)
{
	static int16_t across[MAX_HEIGHT][WIDTH][3];
	for (int y = 0; y < h; y++) {
		int sum[3] = { 0, 0, 0 };
		int src[WIDTH][3];
		for (int x = 0; x < WIDTH; x++) {
			uint32_t c = at_xy(x, y);
			bool hot = emits[y][x] && luma_of(c) >= 110;
			src[x][0] = hot ? (int)(c >> 16 & 255) : 0;
			src[x][1] = hot ? (int)(c >> 8 & 255) : 0;
			src[x][2] = hot ? (int)(c & 255) : 0;
		}
		for (int x = -BLOOM_REACH; x < WIDTH; x++) {
			int in = x + BLOOM_REACH, out = x - BLOOM_REACH - 1;
			for (int c = 0; c < 3; c++) {
				if (in < WIDTH)
					sum[c] += src[in][c];
				if (out >= 0)
					sum[c] -= src[out][c];
			}
			if (x >= 0)
				for (int c = 0; c < 3; c++)
					across[y][x][c] = (int16_t)(sum[c] / (2 * BLOOM_REACH + 1));
		}
	}
	for (int x = 0; x < WIDTH; x++) {
		int sum[3] = { 0, 0, 0 };
		for (int y = -BLOOM_REACH; y < h; y++) {
			int in = y + BLOOM_REACH, out = y - BLOOM_REACH - 1;
			for (int c = 0; c < 3; c++) {
				if (in < h)
					sum[c] += across[in][x][c];
				if (out >= 0)
					sum[c] -= across[out][x][c];
			}
			if (y >= 0)
				for (int c = 0; c < 3; c++)
					bloom[y][x][c] = (int16_t)(sum[c] * 2 / (5 * (2 * BLOOM_REACH + 1)));
		}
	}
}

/* The light at a corner shared by four console pixels, as seen from one surface: the mean of
 * those of the four on it. */
static void corner_light(int x, int y, int here, int h, int out[9])
{
	int n = 0;
	for (int c = 0; c < 9; c++)
		out[c] = 0;
	for (int dy = -1; dy <= 0; dy++)
		for (int dx = -1; dx <= 0; dx++) {
			int yy = y + dy, xx = x + dx;
			if (yy < 0 || xx < 0 || yy >= h || xx >= WIDTH)
				continue;
			for (int c = 0; c < 3; c++)
				out[6 + c] += bloom[yy][xx][c];
			if (surface[yy][xx] != here)
				continue;
			for (int c = 0; c < 6; c++)
				out[c] += lit[yy][xx][c];
			n++;
		}
	for (int c = 0; c < 6; c++)
		out[c] = n ? out[c] * 256 / n : 0;
	for (int c = 6; c < 9; c++)
		out[c] = out[c] * 64;
}

static inline uint32_t lit_color(uint32_t c, const int *v)
{
	int r = ((int)(c >> 16 & 255) * v[0] >> 16) + (v[3] >> 8);
	int g = ((int)(c >> 8 & 255) * v[1] >> 16) + (v[4] >> 8);
	int b = ((int)(c & 255) * v[2] >> 16) + (v[5] >> 8);
	r = r > 255 ? 255 : r;
	g = g > 255 ? 255 : g;
	b = b > 255 ? 255 : b;
	/* the halo lifts what is dark and leaves what is bright, so a fire keeps its drawing */
	r += (v[6] >> 8) * (255 - r) >> 8;
	g += (v[7] >> 8) * (255 - g) >> 8;
	b += (v[8] >> 8) * (255 - b) >> 8;
	return (c & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void apply_rows(void *context, int y0, int y1)
{
	unsigned out_w = ((const Canvas *)context)->out_w;
	int h = ((const Canvas *)context)->height;
	for (int y = y0; y < y1; y++) {
		for (int x = 0; x < WIDTH; x++) {
			bool any = false;
			for (int dy = -1; dy <= 1 && !any; dy++)
				for (int dx = -1; dx <= 1 && !any; dx++) {
					int yy = y + dy, xx = x + dx;
					any = yy >= 0 && xx >= 0 && yy < h && xx < WIDTH && (lit_any[yy][xx] || bloom[yy][xx][0] | bloom[yy][xx][1] | bloom[yy][xx][2]);
				}
			if (!any)
				continue;
			int here = surface[y][x];
			int k[4][9];
			corner_light(x, y, here, h, k[0]);
			corner_light(x + 1, y, here, h, k[1]);
			corner_light(x, y + 1, here, h, k[2]);
			corner_light(x + 1, y + 1, here, h, k[3]);
			/* Bloom lies around a fire, lava or a spell, not on it, and never on a window. */
			bool halo = here != 2 && !emits[y][x];
			int channels = 6;
			if (halo)
				for (int i = 0; i < 4 && channels == 6; i++)
					if (k[i][6] | k[i][7] | k[i][8])
						channels = 9;
			/* One light value per cell of 2x2 screen pixels (a quarter of a console pixel at 8x,
			 * finer than any step shows). The block is the console pixel's one color, so its lit
			 * color is worked out once per cell; only a pixel a shadow changed is lit on its own. */
			int cell = SCALE % 2 == 0 && SCALE >= 4 ? 2 : 1, cells = SCALE / cell;
			int inv = 65536 / cells;
			int lcol[9], rcol[9], ldn[9], rdn[9];
			for (int c = 0; c < channels; c++) {
				ldn[c] = (int)(((long long)(k[2][c] - k[0][c]) * inv) >> 16);
				rdn[c] = (int)(((long long)(k[3][c] - k[1][c]) * inv) >> 16);
				lcol[c] = k[0][c] + ldn[c] / 2;
				rcol[c] = k[1][c] + rdn[c] / 2;
			}
			uint32_t block = grid[y][x];
			for (int cy = 0; cy < cells; cy++) {
				int v[9], step[9];
				for (int c = 0; c < channels; c++) {
					step[c] = (int)(((long long)(rcol[c] - lcol[c]) * inv) >> 16);
					v[c] = lcol[c] + step[c] / 2;
					lcol[c] += ldn[c];
					rcol[c] += rdn[c];
				}
				for (int c = channels; c < 9; c++)
					v[c] = step[c] = 0;
				int at[REMASTER_SCALE_MAX][9];
				uint32_t done[REMASTER_SCALE_MAX];
				for (int cx = 0; cx < cells; cx++) {
					for (int c = 0; c < 9; c++)
						at[cx][c] = v[c] + cx * step[c];
					done[cx] = lit_color(block, at[cx]);
				}
				for (int j = 0; j < cell; j++) {
					uint32_t *p = &image[((size_t)y * SCALE + (unsigned)(cy * cell + j)) * out_w + (size_t)x * SCALE];
					for (int sx = 0; sx < SCALE; sx++)
						p[sx] = p[sx] == block ? done[sx / cell] : lit_color(p[sx], at[sx / cell]);
				}
			}
		}
	}
}

typedef struct {
	const uint32_t *src;
	int w, factor;
	unsigned out_w;
} Nearest;

static void nearest_rows(void *context, int y0, int y1)
{
	const Nearest *n = context;
	for (int y = y0; y < y1; y++) {
		const uint32_t *row = n->src + (size_t)(y * n->factor / SCALE) * (size_t)n->w;
		uint32_t *dst = &image[(size_t)y * n->out_w];
		for (unsigned x = 0; x < n->out_w; x++)
			dst[x] = row[x * (unsigned)n->factor / (unsigned)SCALE];
	}
}

/* Each console pixel a square block: the pixel art stays as drawn. */
static void blocks(unsigned out_w)
{
	Nearest n = { &grid[0][0], WIDTH, 1, out_w };
	remaster_bands((int)src_height * SCALE, nearest_rows, &n);
}

/* A film grade on the scene and its characters: a gentle S curve, cool shadows and warm
 * highlights, richer color, and the corners falling off. Text, windows, the HUD and its
 * bars keep the game's own colors. */
static uint8_t grade_curve[3][256], grade_warm[3][256];
static int vignette_x[WIDTH];

static void grade_init(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	for (int v = 0; v < 256; v++) {
		float t = (float)v / 255.0f;
		float s = t * 0.6f + t * t * (3.0f - 2.0f * t) * 0.4f;
		float low = t * (1.0f - t) * (1.0f - t) * 4.0f, high = t * t * (1.0f - t) * 4.0f;
		float shift[3] = { 0.05f * high - 0.03f * low, 0.015f * high, 0.065f * low - 0.045f * high };
		/* near a fire the shadows keep the light's warmth instead of turning cool, and do not
		 * sink: the light multiplies them, so its pool keeps its reach */
		float warm[3] = { 0.05f * high + 0.03f * low, 0.015f * high + 0.01f * low, -0.045f * high - 0.03f * low };
		for (int c = 0; c < 3; c++) {
			float o = (s + shift[c]) * 255.0f + 0.5f, w = (t + warm[c]) * 255.0f + 0.5f;
			grade_curve[c][v] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o);
			grade_warm[c][v] = (uint8_t)(w < 0 ? 0 : w > 255 ? 255 : w);
		}
	}
	for (int x = 0; x < WIDTH; x++) {
		float d = ((float)x + 0.5f) / (float)WIDTH * 2.0f - 1.0f;
		vignette_x[x] = (int)((1.0f - 0.2f * d * d * d * d) * 256.0f);
	}
}

/* Volume: a figure is rounded, thick in the middle and thin at its edge, so its depth is
 * the distance to its outline. The slope of that depth faces each pixel toward a key light
 * from above: the top of a shoulder or a head catches it, the underside turns away. One
 * shade per console pixel, so the drawing stays square. */
#define VOLUME_DEPTH 7
static uint8_t depth[MAX_HEIGHT][WIDTH];

static bool figure(int x, int y)
{
	return sprite_pixel(x, y) && sprite_body[y][x] >= 24;
}

/* The light a figure takes from the scenery around it: the mean color of what is not a
 * figure within ENV_REACH, as a tint per channel (256 leaves it) at the same brightness. */
#define ENV_REACH 24
static int env_sum[MAX_HEIGHT + 1][WIDTH + 1][4];

static void environment(int h)
{
	for (int x = 0; x <= WIDTH; x++)
		env_sum[0][x][0] = env_sum[0][x][1] = env_sum[0][x][2] = env_sum[0][x][3] = 0;
	for (int y = 0; y < h; y++) {
		int row[4] = { 0, 0, 0, 0 };
		env_sum[y + 1][0][0] = env_sum[y + 1][0][1] = env_sum[y + 1][0][2] = env_sum[y + 1][0][3] = 0;
		for (int x = 0; x < WIDTH; x++) {
			if (!sprite_pixel(x, y)) {
				uint32_t c = grid[y][x];
				row[0] += (int)(c >> 16 & 255), row[1] += (int)(c >> 8 & 255), row[2] += (int)(c & 255), row[3]++;
			}
			for (int k = 0; k < 4; k++)
				env_sum[y + 1][x + 1][k] = env_sum[y][x + 1][k] + row[k];
		}
	}
}

static void env_tint(int x, int y, int h, int tint[3])
{
	int x0 = x - ENV_REACH < 0 ? 0 : x - ENV_REACH, x1 = x + ENV_REACH + 1 > WIDTH ? WIDTH : x + ENV_REACH + 1;
	int y0 = y - ENV_REACH < 0 ? 0 : y - ENV_REACH, y1 = y + ENV_REACH + 1 > h ? h : y + ENV_REACH + 1;
	int s[4];
	for (int k = 0; k < 4; k++)
		s[k] = env_sum[y1][x1][k] - env_sum[y0][x1][k] - env_sum[y1][x0][k] + env_sum[y0][x0][k];
	tint[0] = tint[1] = tint[2] = 256;
	if (s[3] < 64)
		return;
	int r = s[0] / s[3], g = s[1] / s[3], b = s[2] / s[3];
	int l = (r * 77 + g * 150 + b * 29) >> 8;
	if (l < 12)
		return;
	int c3[3] = { r, g, b };
	for (int k = 0; k < 3; k++) {
		int t = c3[k] * 256 / l;
		tint[k] = t < 160 ? 160 : t > 360 ? 360 : t;
	}
}

/* Where the light comes from, as a unit vector on the screen: from above, or toward a fire,
 * lava or window near enough to outshine the sky. With how warm the light reaching there is
 * and how near a light it is. Worked out once every KEY_CELL pixels and blended between. */
#define KEY_CELL 8
#define KEY_COLS (WIDTH / KEY_CELL + 1)
#define KEY_ROWS (MAX_HEIGHT / KEY_CELL + 1)
static float key_field[KEY_ROWS][KEY_COLS][4];  /* direction x, y; warmth 0..256; nearness 0..1 */

static void find_key_field(int h)
{
	for (int cy = 0; cy * KEY_CELL <= h + KEY_CELL && cy < KEY_ROWS; cy++)
		for (int cx = 0; cx < KEY_COLS; cx++) {
			float x = (float)(cx * KEY_CELL), y = (float)(cy * KEY_CELL);
			float ax = -0.35f, ay = -0.8f, best = 0.0f, warm = 0.0f, near = 0.0f;
			for (int i = 0; i < key_count; i++) {
				float dx = key_lights[i].x - x, dy = key_lights[i].y - y;
				float d = __builtin_sqrtf(dx * dx + dy * dy);
				float w = 1.0f - d / (key_lights[i].reach * 1.5f);
				if (w > best && d > 1.0f) {
					best = w;
					ax = -0.35f * (1.0f - w) + dx / d * w;
					ay = -0.8f * (1.0f - w) + dy / d * w;
				}
				float a = 1.0f - d / (key_lights[i].reach * 1.35f);
				if (a > 0.0f) {
					near = a > near ? a : near;
					float wm = a * a * (3.0f - 2.0f * a) * (float)key_lights[i].warmth;
					warm = wm > warm ? wm : warm;
				}
			}
			float ll = __builtin_sqrtf(ax * ax + ay * ay);
			key_field[cy][cx][0] = ax / ll, key_field[cy][cx][1] = ay / ll;
			key_field[cy][cx][2] = warm, key_field[cy][cx][3] = near;
		}
}

static void light_at(int x, int y, float out[4])
{
	int cx = x / KEY_CELL, cy = y / KEY_CELL;
	float fx = (float)(x - cx * KEY_CELL) / KEY_CELL, fy = (float)(y - cy * KEY_CELL) / KEY_CELL;
	cx = cx >= KEY_COLS - 1 ? KEY_COLS - 2 : cx, cy = cy >= KEY_ROWS - 1 ? KEY_ROWS - 2 : cy;
	for (int k = 0; k < 4; k++)
		out[k] = (key_field[cy][cx][k] * (1.0f - fx) + key_field[cy][cx + 1][k] * fx) * (1.0f - fy)
			+ (key_field[cy + 1][cx][k] * (1.0f - fx) + key_field[cy + 1][cx + 1][k] * fx) * fy;
}

static void key_direction(int x, int y, float *lx, float *ly)
{
	float k[4];
	light_at(x, y, k);
	float ll = __builtin_sqrtf(k[0] * k[0] + k[1] * k[1]);
	if (ll < 0.1f)
		k[0] = -0.35f, k[1] = -0.8f, ll = 0.874f;
	*lx = k[0] / ll, *ly = k[1] / ll;
}

static void volume_pass(int h)
{
	find_key_field(h);
	environment(h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++) {
			int d = 0;
			if (figure(x, y)) {
				d = VOLUME_DEPTH;
				if (y > 0 && depth[y - 1][x] + 1 < d)
					d = depth[y - 1][x] + 1;
				if (x > 0 && depth[y][x - 1] + 1 < d)
					d = depth[y][x - 1] + 1;
				if (y == 0 || x == 0)
					d = 1;
			}
			depth[y][x] = (uint8_t)d;
		}
	for (int y = h - 1; y >= 0; y--)
		for (int x = WIDTH - 1; x >= 0; x--) {
			int d = depth[y][x];
			if (!d)
				continue;
			if (y + 1 < h && depth[y + 1][x] + 1 < d)
				d = depth[y + 1][x] + 1;
			if (x + 1 < WIDTH && depth[y][x + 1] + 1 < d)
				d = depth[y][x + 1] + 1;
			if (y + 1 >= h || x + 1 >= WIDTH)
				d = 1;
			depth[y][x] = (uint8_t)d;
		}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++) {
			if (!depth[y][x])
				continue;
			/* the slope over two pixels each side, so a one-pixel notch does not flip it */
			int gx = 0, gy = 0;
			for (int k = 1; k <= 2; k++) {
				gx += (x + k < WIDTH ? depth[y][x + k] : 0) - (x - k >= 0 ? depth[y][x - k] : 0);
				gy += (y + k < h ? depth[y + k][x] : 0) - (y - k >= 0 ? depth[y - k][x] : 0);
			}
			/* outward normal is against the slope; the key light comes from above and a little left */
			float nx = (float)-gx, ny = (float)-gy, nz = 4.0f;
			float len = __builtin_sqrtf(nx * nx + ny * ny + nz * nz);
			float lx, ly;
			key_direction(x, y, &lx, &ly);
			lx *= 0.87f, ly *= 0.87f;
			float facing = (nx * lx + ny * ly + nz * 0.49f) / len - 0.49f;
			int k = (int)(facing * (facing > 0 ? 220.0f : 120.0f));
			uint32_t c = grid[y][x];
			int o[3] = { (int)(c >> 16 & 255), (int)(c >> 8 & 255), (int)(c & 255) };
			int tint[3];
			env_tint(x, y, h, tint);
			/* the whole figure a little in the room's color, the side turned away from the key light more */
			int bounce = 64 + (k < 0 ? -k : 0);
			for (int i = 0; i < 3; i++) {
				o[i] = o[i] * (256 + k) / 256;
				o[i] += (o[i] * tint[i] / 256 - o[i]) * bounce / 256;
				o[i] = o[i] < 0 ? 0 : o[i] > 255 ? 255 : o[i];
			}
			grid[y][x] = (c & 0xff000000u) | (uint32_t)o[0] << 16 | (uint32_t)o[1] << 8 | (uint32_t)o[2];
		}
}

/* Depth: a layer that scrolls slower than the nearest one is farther away (parallax), and so is
 * the backdrop. Far layers sit behind a veil of the scene's own air color, the way distance
 * reads in a painted background. The motion is remembered, so a still moment keeps it. */
static float layer_motion[4];
static uint16_t layer_last[4];
static bool layer_seen;

static void find_depth(int fog[7])
{
	float most = 0;
	for (int layer = 0; layer < 4; layer++) {
		int moved = layer_seen ? abs((int)room_h[layer] - (int)layer_last[layer]) : 0;
		if (moved > 64)
			moved = 0; /* a jump to a new room, not a scroll */
		layer_motion[layer] = layer_motion[layer] * 0.98f + (float)moved;
		layer_last[layer] = room_h[layer];
		most = layer_motion[layer] > most ? layer_motion[layer] : most;
	}
	layer_seen = true;
	for (int layer = 0; layer < 4; layer++) {
		float share = most > 4.0f ? layer_motion[layer] / most : 1.0f;
		fog[layer] = share >= 0.8f ? 0 : share >= 0.4f ? 40 : 72;
	}
	fog[4] = fog[5] = 0;
	fog[6] = 72;
}

/* The scene and its characters, not text, windows or the HUD. */
static uint8_t graded[MAX_HEIGHT][WIDTH];

typedef struct {
	const CoreMotion *m;
	int h, n, fog[7], air[3];
} Grade;

static void grade_rows(void *context, int y0, int y1)
{
	const Grade *gr = context;
	const CoreMotion *m = gr->m;
	int h = gr->h, n = gr->n;
	const int *fog = gr->fog, *air = gr->air;
	for (int y = y0; y < y1; y++) {
		int row = y + m->top;
		float dy = ((float)y + 0.5f) / (float)h * 2.0f - 1.0f;
		int vy = (int)((1.0f - 0.24f * dy * dy * dy * dy) * 256.0f);
		for (int x = 0; x < WIDTH; x++) {
			if (!graded[y][x])
				continue;
			uint32_t c = grid[y][x];
			int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
			int veil = n && !sprite_pixel(x, y) ? fog[m->source[row][x] > 6 ? 6 : m->source[row][x]] : 0;
			/* what a warm light reaches keeps its warmth: no cool shadows, no veil of distance */
			int warm = 0;
			if (key_count) {
				float k[4];
				light_at(x, y, k);
				warm = (int)k[2];
			}
			veil = veil * (256 - warm) >> 8;
			if (veil) {
				r += (air[0] - r) * veil >> 8;
				g += (air[1] - g) * veil >> 8;
				b += (air[2] - b) * veil >> 8;
			}
			if (warm) {
				r = grade_curve[0][r] + ((grade_warm[0][r] - grade_curve[0][r]) * warm >> 8);
				g = grade_curve[1][g] + ((grade_warm[1][g] - grade_curve[1][g]) * warm >> 8);
				b = grade_curve[2][b] + ((grade_warm[2][b] - grade_curve[2][b]) * warm >> 8);
			} else {
				r = grade_curve[0][r], g = grade_curve[1][g], b = grade_curve[2][b];
			}
			int l = (r * 77 + g * 150 + b * 29) >> 8;
			int v = vignette_x[x] * vy;
			int o[3] = { l + ((r - l) * 330 >> 8), l + ((g - l) * 330 >> 8), l + ((b - l) * 330 >> 8) };
			for (int k = 0; k < 3; k++) {
				o[k] = o[k] * v >> 16;
				o[k] = o[k] < 0 ? 0 : o[k] > 255 ? 255 : o[k];
			}
			grid[y][x] = (c & 0xff000000u) | (uint32_t)o[0] << 16 | (uint32_t)o[1] << 8 | (uint32_t)o[2];
		}
	}
}

/* Before the picture is scaled: one console pixel at a time. The corners fall off smoothly
 * enough that a step per console pixel never shows. */
static void grade_pass(const CoreMotion *m, int h)
{
	grade_init();
	find_room(m, h);
	long sum[3] = { 0, 0, 0 }, n = 0;
	for (int y = 0; y < h; y++) {
		int row = y + m->top;
		for (int x = 0; x < WIDTH; x++) {
			int layer = m->source[row][x];
			bool scene = sprite_pixel(x, y) ? sprite_body[y][x] >= 24
				: !light_window(m, row, x) && (layer > 3 || !room_band[layer]) && light_plane(m, row, x);
			graded[y][x] = scene;
			if (scene && !sprite_pixel(x, y)) {
				uint32_t c = grid[y][x];
				sum[0] += c >> 16 & 255, sum[1] += c >> 8 & 255, sum[2] += c & 255;
				n++;
			}
		}
	}
	int fog[7];
	find_depth(fog);
	int air[3] = { 0, 0, 0 };
	if (n) {
		int mean[3] = { (int)(sum[0] / n), (int)(sum[1] / n), (int)(sum[2] / n) };
		int l = (mean[0] * 77 + mean[1] * 150 + mean[2] * 29) >> 8;
		for (int c = 0; c < 3; c++)
			air[c] = (l + mean[c]) / 2 + 36 > 255 ? 255 : (l + mean[c]) / 2 + 36;
	}
	Grade gr = { m, h, (int)n, { 0 }, { air[0], air[1], air[2] } };
	memcpy(gr.fog, fog, sizeof fog);
	remaster_bands(h, grade_rows, &gr);
}

/* The outline of a character against the scenery is drawn as a line instead of a staircase:
 * the figure's shape is read between the centers of its console pixels and cut along where it
 * crosses half, with one screen pixel of soft edge. A straight side stays where it was; a slope
 * of any steepness becomes a slope. Inside the figure and in the scenery every pixel stays square,
 * and a one-pixel line or tip keeps its square ends. */
static uint8_t outline_fig[MAX_HEIGHT][WIDTH];

static void silhouette_rows(void *context, int y0, int y1)
{
	unsigned out_w = ((const Canvas *)context)->out_w;
	int h = ((const Canvas *)context)->height;
	float inv = 1.0f / (float)SCALE;
	for (int j = y0; j < y1 && j + 1 < h; j++)
		for (int i = 0; i + 1 < WIDTH; i++) {
			int a = outline_fig[j][i], b = outline_fig[j][i + 1], c = outline_fig[j + 1][i], d = outline_fig[j + 1][i + 1];
			if ((a == 0) == (b == 0) && (a == 0) == (c == 0) && (a == 0) == (d == 0))
				continue;
			if (a == 2 || b == 2 || c == 2 || d == 2)
				continue;
			/* the dual cell between the four pixel centers */
			const int cx[4] = { i, i + 1, i, i + 1 }, cy[4] = { j, j, j + 1, j + 1 };
			const int in[4] = { a != 0, b != 0, c != 0, d != 0 };
			int half = SCALE / 2;
			for (int sy = 0; sy < SCALE; sy++) {
				float fy = ((float)sy + 0.5f) * inv;
				int py = j * SCALE + half + sy;
				uint32_t *line = &image[(size_t)py * out_w];
				for (int sx = 0; sx < SCALE; sx++) {
					float fx = ((float)sx + 0.5f) * inv;
					float mask = (in[0] * (1.0f - fx) + in[1] * fx) * (1.0f - fy) + (in[2] * (1.0f - fx) + in[3] * fx) * fy;
					float cover = (mask - 0.5f) * (float)SCALE + 0.5f;
					cover = cover < 0.0f ? 0.0f : cover > 1.0f ? 1.0f : cover;
					int near = (fx >= 0.5f) + 2 * (fy >= 0.5f);
					if ((in[near] && cover >= 1.0f) || (!in[near] && cover <= 0.0f))
						continue;
					/* the colors of the nearest figure pixel and the nearest scenery pixel */
					int fig = -1, bg = -1;
					float fd = 9.0f, bd = 9.0f;
					for (int k = 0; k < 4; k++) {
						float ddx = fx - (float)(k & 1), ddy = fy - (float)(k >> 1), dd = ddx * ddx + ddy * ddy;
						if (in[k] && dd < fd)
							fd = dd, fig = k;
						if (!in[k] && dd < bd)
							bd = dd, bg = k;
					}
					uint32_t f = grid[cy[fig]][cx[fig]], g = grid[cy[bg]][cx[bg]];
					int t = (int)(cover * 256.0f);
					int r = (int)(g >> 16 & 255) + (((int)(f >> 16 & 255) - (int)(g >> 16 & 255)) * t >> 8);
					int gg = (int)(g >> 8 & 255) + (((int)(f >> 8 & 255) - (int)(g >> 8 & 255)) * t >> 8);
					int bl = (int)(g & 255) + (((int)(f & 255) - (int)(g & 255)) * t >> 8);
					int px = i * SCALE + half + sx;
					line[px] = (line[px] & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)bl;
				}
			}
		}
}

static void silhouette(int h, unsigned out_w)
{
	if (SCALE < 4)
		return;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++)
			outline_fig[y][x] = figure(x, y);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++) {
			if (!outline_fig[y][x])
				continue;
			int n = (x > 0 && outline_fig[y][x - 1]) + (x + 1 < WIDTH && outline_fig[y][x + 1])
				+ (y > 0 && outline_fig[y - 1][x]) + (y + 1 < h && outline_fig[y + 1][x]);
			if (n <= 1)
				outline_fig[y][x] = 2; /* a tip: kept square */
		}
	Canvas canvas = { out_w, (unsigned)h * SCALE, h };
	remaster_bands(h, silhouette_rows, &canvas);
}

/* Water named in the list: a sea or a lake is a wide sheet of it. A small patch of the same
 * blue tiles is a roof or a banner, and stays as it is. */
#define WATER_LEAST 400
static void find_water(int h)
{
	static uint8_t seen[MAX_HEIGHT][WIDTH];
	memset(seen, 0, sizeof seen);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++) {
			if (!water[y][x] || seen[y][x])
				continue;
			int top = 0, first = 0;
			light_stack[top++] = y * WIDTH + x;
			seen[y][x] = 1;
			while (first < top) {
				int pos = light_stack[first++];
				int cy = pos / WIDTH, cx = pos - cy * WIDTH;
				static const int ny[4] = { 1, -1, 0, 0 }, nx[4] = { 0, 0, 1, -1 };
				for (int k = 0; k < 4; k++) {
					int yy = cy + ny[k], xx = cx + nx[k];
					if (yy < 0 || xx < 0 || yy >= h || xx >= WIDTH || !water[yy][xx] || seen[yy][xx])
						continue;
					seen[yy][xx] = 1;
					light_stack[top++] = yy * WIDTH + xx;
				}
			}
			if (top < WATER_LEAST)
				for (int i = 0; i < top; i++)
					water[light_stack[i] / WIDTH][light_stack[i] % WIDTH] = 0;
		}
}

/* Water: the light of the sky moves over it in slow broad swells, and the crest of each wave
 * the artist drew catches the sun now and then as a glint. Both are fixed to the world, so they
 * travel with the sea when the view scrolls. One color per console pixel. */
static void water_pass(const CoreMotion *m, int h)
{
	float t = (float)m->frame;
	for (int y = 1; y + 1 < h; y++) {
		int row = y + m->top;
		for (int x = 0; x < WIDTH; x++) {
			if (!water[y][x])
				continue;
			int layer = m->source[row][x] > 3 ? 0 : m->source[row][x];
			int wx = x + room_h[layer], wy = y + room_v[layer];
			float swell = __builtin_sinf((float)wx * 0.21f + (float)wy * 0.37f + t * 0.05f)
				+ __builtin_sinf((float)wx * 0.13f - (float)wy * 0.29f - t * 0.037f);
			int k = (int)(swell * 12.0f);
			uint32_t c = grid[y][x];
			int l = luma_of(c);
			bool crest = water[y - 1][x] && water[y + 1][x] && l >= luma_of(grid[y - 1][x]) + 10 && l >= luma_of(grid[y + 1][x]) + 10;
			/* glints only on the bright side of a swell, a short dash along the wave, never a lone star */
			unsigned hash = light_hash((unsigned)(wx >> 1) * 73856093u ^ (unsigned)wy * 19349663u);
			int glint = 0;
			if (swell > 0.7f && (crest || hash % 5 == 0)) {
				int phase = (int)((unsigned)m->frame + hash % 61) % 61;
				if (phase < 8)
					glint = (int)((float)(phase < 4 ? phase + 1 : 8 - phase) * (swell - 0.7f) * 45.0f);
			}
			int r = (int)(c >> 16 & 255) * (256 + k) >> 8, g = (int)(c >> 8 & 255) * (256 + k) >> 8, b = (int)(c & 255) * (256 + k) >> 8;
			r += (232 - r) * glint >> 8, g += (240 - g) * glint >> 8, b += (255 - b) * glint >> 8;
			r = r < 0 ? 0 : r > 255 ? 255 : r, g = g < 0 ? 0 : g > 255 ? 255 : g, b = b < 0 ? 0 : b > 255 ? 255 : b;
			grid[y][x] = (c & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
}

/* Shine: the glints the artist drew on metal, gold, glass or a polished floor (a pixel or two
 * much brighter than all around) catch the light as a hard specular, strongest near a fire or
 * a window. Stone and cloth, drawn without such glints, stay matte. One color per console pixel. */
static void shine_pass(int h)
{
	for (int y = 1; y + 1 < h; y++)
		for (int x = 1; x + 1 < WIDTH; x++) {
			if (!graded[y][x] || emits[y][x] || water[y][x])
				continue;
			uint32_t c = grid[y][x];
			int l = luma_of(c);
			if (l < 120)
				continue;
			int around = 0, brighter = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					if (!dx && !dy)
						continue;
					int n = luma_of(grid[y + dy][x + dx]);
					brighter += n >= l - 8;
					around = n > around && n < l - 8 ? n : around;
				}
			if (brighter > 2 || l - around < 48)
				continue; /* part of a bright area, or not standing out: not a glint */
			if ((x >= 2 && grid[y][x - 2] == c) || (x + 2 < WIDTH && grid[y][x + 2] == c)
				|| (y >= 2 && grid[y - 2][x] == c) || (y + 2 < h && grid[y + 2][x] == c))
				continue; /* a row of dots, a pattern: it would sparkle like stars */
			float kf[4];
			light_at(x, y, kf);
			float near = kf[3];
			int k = 50 + (int)(near * 110.0f);
			int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
			r += (255 - r) * k >> 8, g += (255 - g) * k >> 8, b += (255 - b) * k >> 8;
			grid[y][x] = (c & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
}

/* Relief: where two colors of the drawing meet, the lighter one stands a little proud of the
 * darker. Its edge that faces the light catches it and it throws a thin shadow on the other
 * side, so the art reads as carved and the light seems to move over it. Only between two
 * pixels of the scene or two of one figure: never on the outline of a figure against the
 * scenery, never on text, windows or the HUD. */
static void shade_strip(unsigned out_w, int x, int y, int side, int k)
{
	int w = SCALE / 4 < 1 ? 1 : SCALE / 4;
	for (int i = 0; i < w; i++) {
		int kk = k * (w - i) / w;
		for (int j = 0; j < SCALE; j++) {
			int sx = side == 0 ? i : side == 1 ? SCALE - 1 - i : j;
			int sy = side == 2 ? i : side == 3 ? SCALE - 1 - i : j;
			uint32_t *p = &image[((size_t)y * SCALE + (unsigned)sy) * out_w + (size_t)x * SCALE + (unsigned)sx];
			int r = (int)(*p >> 16 & 255) * (256 + kk) >> 8, g = (int)(*p >> 8 & 255) * (256 + kk) >> 8, b = (int)(*p & 255) * (256 + kk) >> 8;
			r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
			*p = (*p & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
}

static int8_t relief_k[MAX_HEIGHT][WIDTH][4];

static void relief_rows(void *context, int y0, int y1)
{
	unsigned out_w = *(const unsigned *)context;
	for (int y = y0; y < y1; y++)
		for (int x = 0; x < WIDTH; x++)
			for (int s = 0; s < 4; s++)
				if (relief_k[y][x][s])
					shade_strip(out_w, x, y, s, relief_k[y][x][s]);
}

static void relief(int h, unsigned out_w)
{
	if (SCALE < 4)
		return;
	/* what can take relief: the scene and its figures (1) or (2), with the brightness of each */
	static uint8_t kind[MAX_HEIGHT][WIDTH], lum[MAX_HEIGHT][WIDTH];
	for (int y = 0; y < h; y++)
		for (int x = 0; x < WIDTH; x++) {
			kind[y][x] = !graded[y][x] || emits[y][x] || water[y][x] ? 0 : sprite_pixel(x, y) ? 2 : 1;
			lum[y][x] = (uint8_t)luma_of(grid[y][x]);
		}
	memset(relief_k, 0, sizeof relief_k);
	/* left, right, up, down */
	static const int sdx[4] = { -1, 1, 0, 0 }, sdy[4] = { 0, 0, -1, 1 };
	for (int y = 1; y + 1 < h; y++)
		for (int x = 1; x + 1 < WIDTH; x++) {
			int here = kind[y][x];
			if (!here)
				continue;
			int l = lum[y][x];
			uint32_t c = grid[y][x];
			float lx = 0, ly = 0;
			bool known = false;
			for (int s = 0; s < 4; s++) {
				int nx = x + sdx[s], ny = y + sdy[s];
				int diff = l - lum[ny][nx];
				if (diff < 16 || kind[ny][nx] != here)
					continue; /* the lower of the two or alike, or another surface: not this one's edge */
				/* an edge of a shape, two pixels long at least, not a speck of dither */
				bool along = s < 2 ? grid[y - 1][x] == c || grid[y + 1][x] == c : grid[y][x - 1] == c || grid[y][x + 1] == c;
				if (!along)
					continue;
				if (!known) {
					key_direction(x, y, &lx, &ly);
					known = true;
				}
				float facing = (float)sdx[s] * lx + (float)sdy[s] * ly;
				/* by contrast against the darker side, so a dim room shows it as much as a bright one */
				int k = diff * 160 / (l - diff + 48);
				k = k > 100 ? 100 : k;
				if (facing > 0.2f)
					relief_k[y][x][s] = (int8_t)(k * facing);
				else if (facing < -0.2f)
					relief_k[ny][nx][s ^ 1] = (int8_t)(k * facing * 1.2f);
			}
		}
	remaster_bands(h, relief_rows, &out_w);
}

/* Each character throws a soft round shadow on the ground it stands on, darkest under the
 * feet and fading to nothing, on the scenery only: never on a figure, a window or a glow. */
static void ground_shadows(const CoreMotion *m, unsigned out_w, unsigned out_h)
{
	for (int i = 0; i < feet_count; i++) {
		const Feet *f = &feet[i];
		float cx = (float)(f->x0 + f->x1 + 1) * 0.5f * (float)SCALE;
		float cy = ((float)f->y + 0.7f) * (float)SCALE;
		float rx = (float)(f->x1 - f->x0 + 1) * 0.85f * (float)SCALE;
		if (rx < 7.0f * (float)SCALE)
			rx = 7.0f * (float)SCALE;
		float ry = rx * 0.3f;
		int x0 = (int)(cx - rx), x1 = (int)(cx + rx), y0 = (int)(cy - ry), y1 = (int)(cy + ry);
		x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
		x1 = x1 >= (int)out_w ? (int)out_w - 1 : x1, y1 = y1 >= (int)out_h ? (int)out_h - 1 : y1;
		for (int sy = y0 / SCALE; sy <= y1 / SCALE; sy++) {
			int row = sy + m->top;
			for (int sx = x0 / SCALE; sx <= x1 / SCALE; sx++) {
				if (sprite_pixel(sx, sy) || m->source[row][sx] > 3 || light_window(m, row, sx) || emitter(m, sy, sx))
					continue;
				for (int py = sy * SCALE; py < (sy + 1) * SCALE; py++) {
					float dy = ((float)py + 0.5f - cy) / ry;
					uint32_t *line = &image[(size_t)py * out_w];
					for (int px = sx * SCALE; px < (sx + 1) * SCALE; px++) {
						float dx = ((float)px + 0.5f - cx) / rx, d = dx * dx + dy * dy;
						if (d >= 1.0f)
							continue;
						float a = (1.0f - d) * (1.0f - d);
						int k = (int)(a * 130.0f);
						uint32_t c = line[px];
						int r = (int)(c >> 16 & 255) * (256 - k) >> 8, g = (int)(c >> 8 & 255) * (256 - k * 15 / 16) >> 8;
						int b = (int)(c & 255) * (256 - k * 7 / 8) >> 8;
						line[px] = (c & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
					}
				}
			}
		}
	}
}

static int scale_hint;

void remaster_use_scale(int scale)
{
	scale_hint = scale;
}

const uint32_t *remaster_frame(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m,
	unsigned *width, unsigned *out_height)
{
	if (!pixels || !m || !m->valid || height == 0 || height > MAX_HEIGHT || height + m->top > MAX_HEIGHT)
		return NULL;

	src_frame = pixels;
	src_pitch = pitch;
	src_motion = m;
	src_height = height;

	int fit = scale_hint > 0 ? scale_hint : video_fit_scale();
	if (fit < 1)
		fit = 1;
	if (fit > REMASTER_SCALE_MAX)
		fit = REMASTER_SCALE_MAX;
	scale = fit;

	unsigned out_w = WIDTH * SCALE;
	unsigned out_h = height * SCALE;

	prepare_grid();
	scan_listed(m, (int)height);
	volume_pass((int)height);
	grade_pass(m, (int)height);
	water_pass(m, (int)height);
	shine_pass((int)height);
	blocks(out_w);
	silhouette((int)height, out_w);
	ground_shadows(m, out_w, out_h);

	memset(shadowed, 0, (size_t)out_w * out_h);
	for (unsigned y = 0; y + 1 < height; y++) {
		int row = (int)y + m->top;
		int below = row + 1;
		for (int x = 0; x < WIDTH; x++) {
			if (!sprite_key(key_at(m, row, x)))
				continue;
			if (sprite_key(key_at(m, below, x)) || key_at(m, below, x) > 3)
				continue; /* shadow lands on a background layer, not on sky or another sprite */
			int rows = SCALE / 3;
			if (rows < 1)
				rows = 1;
			/* A flat contact, not a console-pixel slab. The end of the run steps
			 * one screen pixel so the corner is not a square block. */
			bool left_open = x == 0
				|| !sprite_key(key_at(m, row, x - 1))
				|| sprite_key(key_at(m, below, x - 1))
				|| key_at(m, below, x - 1) > 3;
			bool right_open = x + 1 >= WIDTH
				|| !sprite_key(key_at(m, row, x + 1))
				|| sprite_key(key_at(m, below, x + 1))
				|| key_at(m, below, x + 1) > 3;
			uint32_t *foot = &image[((size_t)(y + 1) * SCALE - 1) * out_w + (size_t)x * SCALE];
			uint32_t ground[REMASTER_SCALE_MAX];
			uint32_t *grow = &image[(size_t)(y + 1) * SCALE * out_w + (size_t)x * SCALE];
			for (int sx = 0; sx < SCALE; sx++)
				ground[sx] = grow[sx];
			for (int sx = 0; sx < SCALE; sx++) {
				if (foot[sx] == ground[sx])
					continue;
				int start = 0;
				while (start < SCALE && shadowed[(size_t)((y + 1) * SCALE + start) * out_w + (size_t)x * SCALE + (unsigned)sx])
					start++;
				int cut = start + rows;
				if (cut > SCALE)
					cut = SCALE;
				if (left_open && sx + 1 < cut)
					cut = sx + 1;
				if (right_open && SCALE - sx < cut)
					cut = SCALE - sx;
				for (int j = start; j < cut; j++) {
					unsigned py = (y + 1) * SCALE + (unsigned)j;
					if (py >= out_h)
						continue;
					int px = x * SCALE + sx;
					if (sprite_key(key_at(m, (int)py / SCALE + m->top, px / SCALE)) || emitter(m, (int)py / SCALE, px / SCALE))
						continue;
					size_t at = (size_t)py * out_w + (unsigned)px;
					if (shadowed[at])
						continue;
					shadowed[at] = 1;
					image[at] = shade(image[at], 52);
				}
			}
		}
	}

	light_pass(m, height, out_w);
	relief((int)height, out_w);
	*width = out_w;
	*out_height = out_h;
	return image;
}
