#include "hdpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define WIDTH 256
#define MAX_HEIGHT 240
#define TILE (8 * HDPACK_SCALE)
#define VRAM_MASK 0x7fff
#define CAPTURE_MIN_NEW 256  /* new texels a frame must show to be saved */
#define CAPTURE_GAP 10       /* frames between two saved frames */
#define PATH 4096

/* Pack file: "RMXHD1\0\0", u32 scale, u32 count, then count times: u64 key, (8 * scale)^2 u32
 * pixels 0x00RRGGBB at full brightness. Frame file (gzip): "RMXF1\0\0\0", u32 width, u32 height,
 * u32 pixels[height][width], u64 keys[height][width] (0: not clean), u8 texels[height][width], u32
 * tile count, then per tile: u64 key, u32 colors[64] (0xff000000: transparent). */

typedef struct {
	uint64_t key;
	uint64_t value;  /* pack: index of the image; capture: texels already seen (bit x | y << 3) */
} Entry;

typedef struct {
	Entry *entries;
	size_t mask, used;
} Table;

static uint64_t *table_find(const Table *t, uint64_t key)
{
	if (!t->entries)
		return NULL;
	for (size_t i = key & t->mask;; i = (i + 1) & t->mask) {
		if (t->entries[i].key == key)
			return &t->entries[i].value;
		if (!t->entries[i].key)
			return NULL;
	}
}

static uint64_t *table_insert(Table *t, uint64_t key)
{
	if ((t->used + 1) * 2 > t->mask + 1 || !t->entries) {
		Table bigger = { calloc(t->entries ? (t->mask + 1) * 2 : 1024, sizeof(Entry)), t->entries ? t->mask * 2 + 1 : 1023, 0 };
		for (size_t i = 0; t->entries && i <= t->mask; i++)
			if (t->entries[i].key)
				*table_insert(&bigger, t->entries[i].key) = t->entries[i].value;
		free(t->entries);
		*t = bigger;
	}
	size_t i = key & t->mask;
	while (t->entries[i].key && t->entries[i].key != key)
		i = (i + 1) & t->mask;
	if (!t->entries[i].key)
		t->entries[i].key = key, t->used++;
	return &t->entries[i].value;
}

/* ---- tile identity ---- */

static uint32_t render_stamp;
static uint64_t tile_hash[3][VRAM_MASK + 1];
static uint32_t tile_stamp[3][VRAM_MASK + 1];

static uint64_t fnv(uint64_t h, uint32_t v)
{
	return (h ^ v) * 0x100000001b3ULL;
}

static unsigned tile_words(int format)
{
	return 8u << (format - 1);  /* 2bpp: 8 words, 4bpp: 16, 8bpp: 32 */
}

static uint64_t data_hash(const CoreMotion *m, int format, unsigned base)
{
	if (tile_stamp[format - 1][base] == render_stamp)
		return tile_hash[format - 1][base];
	uint64_t h = fnv(0xcbf29ce484222325ULL, (uint32_t)format);
	for (unsigned i = 0; i < tile_words(format); i++)
		h = fnv(h, m->vram[(base + i) & VRAM_MASK]);
	tile_stamp[format - 1][base] = render_stamp;
	tile_hash[format - 1][base] = h;
	return h;
}

static uint64_t palette_hash(const CoreMotion *m, int row, int format, unsigned palette)
{
	unsigned colors = 1u << (2 << (format - 1));  /* 4, 16, 256 */
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < colors; i++)
		h = fnv(h, m->cgram[row][(palette + i) & 0xff] & 0x7fff);
	return h;
}

/* Palette hashes of the current line, by format and palette start. */
static uint64_t row_palette[3][256];
static bool row_palette_ok[3][256];

static void start_row(void)
{
	memset(row_palette_ok, 0, sizeof row_palette_ok);
}

static uint64_t pixel_key(const CoreMotion *m, int row, int x)
{
	int format = m->format[row][x];
	if (!format || format > 3)
		return 0;
	unsigned palette = m->palette[row][x];
	if (!row_palette_ok[format - 1][palette]) {
		row_palette[format - 1][palette] = palette_hash(m, row, format, palette);
		row_palette_ok[format - 1][palette] = true;
	}
	uint64_t key = data_hash(m, format, m->tile[row][x] & VRAM_MASK) ^ row_palette[format - 1][palette] * 0x9e3779b97f4a7c15ULL;
	return key ? key : 1;
}

/* ---- the pack ---- */

static Table pack;
static uint32_t *images;
static uint32_t out[MAX_HEIGHT * HDPACK_SCALE * WIDTH * HDPACK_SCALE];

bool hdpack_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	char magic[8];
	uint32_t scale, count;
	bool ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, "RMXHD1\0\0", 8) && fread(&scale, 4, 1, f) == 1
		&& scale == HDPACK_SCALE && fread(&count, 4, 1, f) == 1 && count > 0;
	if (ok) {
		images = malloc((size_t)count * TILE * TILE * sizeof(uint32_t));
		for (uint32_t i = 0; ok && i < count; i++) {
			uint64_t key;
			ok = images && fread(&key, 8, 1, f) == 1 && fread(images + (size_t)i * TILE * TILE, sizeof(uint32_t) * TILE * TILE, 1, f) == 1;
			if (ok)
				*table_insert(&pack, key) = i;
		}
	}
	fclose(f);
	if (!ok) {
		free(images), images = NULL;
		free(pack.entries), memset(&pack, 0, sizeof pack);
		fprintf(stderr, "pacote de arte HD inválido: %s\n", path);
	}
	return ok;
}

bool hdpack_loaded(void)
{
	return images != NULL;
}

static uint32_t channel_mix(uint32_t final, uint32_t art, uint32_t own, unsigned brightness, int shift)
{
	int lit = (int)((art >> shift & 0xff) * brightness / 15);
	int v = (int)(final >> shift & 0xff) + lit - (int)(own >> shift & 0xff);
	return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << shift;
}

const uint32_t *hdpack_render(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m,
	unsigned *width, unsigned *out_height)
{
	if (!images || height > MAX_HEIGHT)
		return NULL;
	render_stamp++;
	unsigned out_w = WIDTH * HDPACK_SCALE, replaced = 0;
	for (unsigned y = 0; y < height; y++) {
		const uint32_t *line = (const uint32_t *)((const char *)pixels + y * pitch);
		int row = (int)y + m->top;
		unsigned brightness = m->brightness[row];
		start_row();
		for (unsigned x = 0; x < WIDTH; x++) {
			uint32_t final = line[x] & 0xffffff;
			uint32_t *block = out + (size_t)y * HDPACK_SCALE * out_w + x * HDPACK_SCALE;
			uint64_t key = pixel_key(m, row, (int)x), *index = key ? table_find(&pack, key) : NULL;
			if (!index) {
				for (int j = 0; j < HDPACK_SCALE; j++)
					for (int i = 0; i < HDPACK_SCALE; i++)
						block[j * out_w + i] = final;
				continue;
			}
			replaced++;
			const uint32_t *art = images + *index * TILE * TILE;
			uint8_t texel = m->texel[row][x];
			int tx = (texel & 7) * HDPACK_SCALE, ty = (texel >> 3 & 7) * HDPACK_SCALE;
			bool hflip = texel & 0x40, vflip = texel & 0x80;
			uint32_t own = m->color[row][x] & 0xffffff;
			for (int j = 0; j < HDPACK_SCALE; j++)
				for (int i = 0; i < HDPACK_SCALE; i++) {
					int sx = hflip ? HDPACK_SCALE - 1 - i : i, sy = vflip ? HDPACK_SCALE - 1 - j : j;
					uint32_t c = art[(ty + sy) * TILE + tx + sx];
					block[j * out_w + i] = channel_mix(final, c, own, brightness, 16)
						| channel_mix(final, c, own, brightness, 8) | channel_mix(final, c, own, brightness, 0);
				}
		}
	}
	if (!replaced)
		return NULL;
	*width = out_w;
	*out_height = height * HDPACK_SCALE;
	return out;
}

/* ---- capture ---- */

static char capture_dir[PATH];
static Table coverage;
static unsigned capture_files, frames_since_capture = CAPTURE_GAP;
static uint64_t frame_keys[MAX_HEIGHT * WIDTH];
static uint8_t frame_texels[MAX_HEIGHT * WIDTH];

static uint32_t rgb555(uint16_t c)
{
	unsigned r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
	return (r * 255 + 15) / 31 << 16 | (g * 255 + 15) / 31 << 8 | (b * 255 + 15) / 31;
}

/* The tile's 8x8 pixels at full brightness, as the console would draw them unflipped. */
static void decode_tile(const CoreMotion *m, int row, int x, uint32_t colors[64])
{
	int format = m->format[row][x];
	unsigned base = m->tile[row][x], palette = m->palette[row][x];
	for (int cy = 0; cy < 8; cy++)
		for (int cx = 0; cx < 8; cx++) {
			unsigned index = 0;
			for (int plane = 0; plane < (2 << (format - 1)); plane += 2) {
				uint16_t word = m->vram[(base + (unsigned)plane * 4 + (unsigned)cy) & VRAM_MASK];
				index |= (unsigned)(word >> (7 - cx) & 1) << plane;
				index |= (unsigned)(word >> (15 - cx) & 1) << (plane + 1);
			}
			colors[cy * 8 + cx] = index ? rgb555(m->cgram[row][(palette + index) & 0xff]) : 0xff000000u;
		}
}

void hdpack_capture_init(const char *dir)
{
	snprintf(capture_dir, sizeof capture_dir, "%s", dir);
	char path[PATH + 32];
	snprintf(path, sizeof path, "%s/coverage.bin", dir);
	FILE *f = fopen(path, "rb");
	Entry e;
	while (f && fread(&e, sizeof e, 1, f) == 1)
		*table_insert(&coverage, e.key) = e.value;
	if (f)
		fclose(f);
	for (;; capture_files++) {
		snprintf(path, sizeof path, "%s/frame-%06u.rmxf", dir, capture_files);
		FILE *g = fopen(path, "rb");
		if (!g)
			break;
		fclose(g);
	}
}

void hdpack_capture_save(void)
{
	if (!capture_dir[0] || !coverage.entries)
		return;
	char path[PATH + 32], tmp[PATH + 48];
	snprintf(path, sizeof path, "%s/coverage.bin", capture_dir);
	snprintf(tmp, sizeof tmp, "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	for (size_t i = 0; i <= coverage.mask; i++)
		if (coverage.entries[i].key)
			fwrite(&coverage.entries[i], sizeof(Entry), 1, f);
	fclose(f);
	rename(tmp, path);
}

void hdpack_capture(const void *pixels, size_t pitch, unsigned height, const CoreMotion *m)
{
	if (!capture_dir[0] || height > MAX_HEIGHT || ++frames_since_capture < CAPTURE_GAP)
		return;
	render_stamp++;
	unsigned fresh = 0;
	for (unsigned y = 0; y < height; y++) {
		const uint32_t *line = (const uint32_t *)((const char *)pixels + y * pitch);
		int row = (int)y + m->top;
		bool bright = m->brightness[row] == 15;
		start_row();
		for (unsigned x = 0; x < WIDTH; x++) {
			size_t at = (size_t)y * WIDTH + x;
			uint64_t key = bright && (line[x] & 0xffffff) == (m->color[row][x] & 0xffffff) ? pixel_key(m, row, (int)x) : 0;
			if (key && table_find(&pack, key))
				key = 0;  /* already redrawn */
			frame_keys[at] = key;
			frame_texels[at] = m->texel[row][x];
			uint64_t *seen = key ? table_find(&coverage, key) : NULL;
			if (key && (!seen || !(*seen >> (m->texel[row][x] & 63) & 1)))
				fresh++;
		}
	}
	if (fresh < CAPTURE_MIN_NEW)
		return;
	frames_since_capture = 0;

	char path[PATH + 32];
	snprintf(path, sizeof path, "%s/frame-%06u.rmxf", capture_dir, capture_files++);
	gzFile f = gzopen(path, "wb6");
	if (!f)
		return;
	uint32_t header[2] = { WIDTH, height };
	gzwrite(f, "RMXF1\0\0\0", 8);
	gzwrite(f, header, sizeof header);
	for (unsigned y = 0; y < height; y++) {
		uint32_t line[WIDTH];
		const uint32_t *source = (const uint32_t *)((const char *)pixels + y * pitch);
		for (unsigned x = 0; x < WIDTH; x++)
			line[x] = source[x] & 0xffffff;
		gzwrite(f, line, sizeof line);
	}
	gzwrite(f, frame_keys, (unsigned)(height * WIDTH * sizeof(uint64_t)));
	gzwrite(f, frame_texels, height * WIDTH);

	/* Every tile in the frame once, with its original pixels. */
	Table in_frame = { 0 };
	uint32_t tiles = 0;
	for (unsigned at = 0; at < height * WIDTH; at++)
		if (frame_keys[at] && !table_find(&in_frame, frame_keys[at]))
			*table_insert(&in_frame, frame_keys[at]) = at, tiles++;
	gzwrite(f, &tiles, sizeof tiles);
	for (size_t i = 0; in_frame.entries && i <= in_frame.mask; i++) {
		if (!in_frame.entries[i].key)
			continue;
		unsigned at = (unsigned)in_frame.entries[i].value;
		uint32_t colors[64];
		decode_tile(m, (int)(at / WIDTH) + m->top, (int)(at % WIDTH), colors);
		gzwrite(f, &in_frame.entries[i].key, sizeof(uint64_t));
		gzwrite(f, colors, sizeof colors);
	}
	free(in_frame.entries);
	gzclose(f);

	for (unsigned at = 0; at < height * WIDTH; at++)
		if (frame_keys[at])
			*table_insert(&coverage, frame_keys[at]) |= 1ULL << (frame_texels[at] & 63);
}
