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

#define BANDS 8
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
/* The neighbor's shaded color, sampled before any stair. A later stair must not
 * copy a corner a previous stair already replaced. */
static uint32_t stair_color[MAX_HEIGHT][WIDTH];

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


/* Run length of each console pixel, capped. A letter is one pixel wide, so it
 * never reaches 3. Built once per frame: walking the run again from every pixel
 * was most of the cost. */
static uint32_t grid[MAX_HEIGHT][WIDTH];
static uint8_t wide_run[MAX_HEIGHT][WIDTH];
static bool field[MAX_HEIGHT][WIDTH]; /* a flat color across half the screen */
static uint8_t tall_run[MAX_HEIGHT][WIDTH];
static uint8_t run_at[MAX_HEIGHT][WIDTH]; /* rows down from the top of the color run */
/* Where this pixel sits in the sprite silhouette, not in a run of one color.
 * A character is many colors wide; a letter is one pixel wide. */
static uint8_t sprite_above[MAX_HEIGHT][WIDTH];
static uint8_t sprite_below[MAX_HEIGHT][WIDTH];
static uint8_t sprite_left[MAX_HEIGHT][WIDTH];
static uint8_t sprite_right[MAX_HEIGHT][WIDTH];
/* Height of the connected sprite. A letter is a short shape; a character is tall,
 * so a one-pixel hair column still belongs to the character. */
static uint8_t sprite_body[MAX_HEIGHT][WIDTH];
/* A warm cliff is many colors. The column of the body, not of one stripe, carries the light. */
static uint8_t warm_above[MAX_HEIGHT][WIDTH];
static uint8_t warm_below[MAX_HEIGHT][WIDTH];
/* A letter of any width, with the one-pixel outline around it: a small white shape with a dark
 * outline or drop shadow on at least a quarter of its border, on a window that is not dark. It
 * keeps every pixel the game drew. */
static bool glyph[MAX_HEIGHT][WIDTH];

static bool sprite_pixel(int x, int y);
static bool warm_body(uint32_t color);
static int luma_of(uint32_t color);

static bool letter_ink(uint32_t color)
{
	int r = (int)(color >> 16 & 255), g = (int)(color >> 8 & 255), b = (int)(color & 255);
	int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	return lo >= 180 && hi - lo <= 24;
}

static void find_glyphs(int height)
{
	enum { MAX_W = 12, MAX_H = 16, MAX_INK = 120 };
	static uint8_t seen[MAX_HEIGHT][WIDTH];
	static int stack[WIDTH * MAX_HEIGHT], marks[WIDTH * MAX_HEIGHT];
	memset(seen, 0, sizeof seen);
	memset(glyph, 0, sizeof glyph);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (seen[y][x] || !letter_ink(grid[y][x]))
				continue;
			int top = 0, count = 0;
			int min_x = x, max_x = x, min_y = y, max_y = y;
			stack[top++] = y * WIDTH + x;
			seen[y][x] = 1;
			while (top > 0) {
				int pos = stack[--top];
				marks[count++] = pos;
				int cy = pos / WIDTH, cx = pos - cy * WIDTH;
				min_x = cx < min_x ? cx : min_x, max_x = cx > max_x ? cx : max_x;
				min_y = cy < min_y ? cy : min_y, max_y = cy > max_y ? cy : max_y;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int ny = cy + dy, nx = cx + dx;
						if (ny < 0 || nx < 0 || ny >= height || nx >= WIDTH || seen[ny][nx] || !letter_ink(grid[ny][nx]))
							continue;
						seen[ny][nx] = 1;
						stack[top++] = ny * WIDTH + nx;
					}
			}
			if (max_x - min_x >= MAX_W || max_y - min_y >= MAX_H || count > MAX_INK)
				continue;
			bool character = false;
			for (int i = 0; i < count && !character; i++) {
				int cy = marks[i] / WIDTH, cx = marks[i] - cy * WIDTH;
				character = sprite_pixel(cx, cy) && sprite_body[cy][cx] >= 14;
			}
			if (character)
				continue; /* a highlight on a character, who is lit as a body */
			int border = 0, dark = 0;
			for (int i = 0; i < count; i++) {
				int cy = marks[i] / WIDTH, cx = marks[i] - cy * WIDTH;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int ny = cy + dy, nx = cx + dx;
						if (ny < 0 || nx < 0 || ny >= height || nx >= WIDTH || letter_ink(grid[ny][nx]))
							continue;
						border++;
						dark += luma_of(grid[ny][nx]) < 70;
					}
			}
			if (border == 0 || dark * 4 < border)
				continue;
			/* Past the outline a letter sits on its window. Art in a black void has only dark there. */
			int panel = 0, lit = 0;
			for (int i = 0; i < count; i++) {
				int cy = marks[i] / WIDTH, cx = marks[i] - cy * WIDTH;
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++) {
						int ny = cy + dy, nx = cx + dx;
						if ((dy > -2 && dy < 2 && dx > -2 && dx < 2) || ny < 0 || nx < 0 || ny >= height || nx >= WIDTH
							|| letter_ink(grid[ny][nx]))
							continue;
						panel++;
						lit += luma_of(grid[ny][nx]) >= 28;
					}
			}
			if (lit * 10 < panel * 3)
				continue;
			for (int i = 0; i < count; i++) {
				int cy = marks[i] / WIDTH, cx = marks[i] - cy * WIDTH;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int ny = cy + dy, nx = cx + dx;
						if (ny >= 0 && nx >= 0 && ny < height && nx < WIDTH)
							glyph[ny][nx] = true;
					}
			}
		}
	}
}

static void prepare_grid(void)
{
	int height = (int)src_height;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < WIDTH; x++)
			grid[y][x] = at_xy(x, y);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH;) {
			int end = x + 1;
			while (end < WIDTH && end - x < 255 && grid[y][end] == grid[y][x])
				end++;
			uint8_t len = (uint8_t)(end - x);
			for (int i = x; i < end; i++)
				wide_run[y][i] = len;
			x = end;
		}
	}
	for (int x = 0; x < WIDTH; x++) {
		for (int y = 0; y < height;) {
			int end = y + 1;
			while (end < height && end - y < 255 && grid[end][x] == grid[y][x])
				end++;
			uint8_t len = (uint8_t)(end - y);
			for (int i = y; i < end; i++) {
				tall_run[i][x] = len;
				run_at[i][x] = (uint8_t)(i - y);
			}
			y = end;
		}
	}
	for (int x = 0; x < WIDTH; x++) {
		for (int y = 0; y < height;) {
			if (!sprite_pixel(x, y)) {
				sprite_above[y][x] = sprite_below[y][x] = 0;
				y++;
				continue;
			}
			int end = y + 1;
			while (end < height && sprite_pixel(x, end))
				end++;
			for (int i = y; i < end; i++) {
				sprite_above[i][x] = (uint8_t)(i - y);
				sprite_below[i][x] = (uint8_t)(end - 1 - i);
			}
			y = end;
		}
	}
	for (int x = 0; x < WIDTH; x++) {
		for (int y = 0; y < height;) {
			if (!warm_body(grid[y][x])) {
				warm_above[y][x] = warm_below[y][x] = 0;
				y++;
				continue;
			}
			int end = y + 1;
			while (end < height && warm_body(grid[end][x]))
				end++;
			for (int i = y; i < end; i++) {
				warm_above[i][x] = (uint8_t)(i - y);
				warm_below[i][x] = (uint8_t)(end - 1 - i);
			}
			y = end;
		}
	}
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH;) {
			if (!sprite_pixel(x, y)) {
				sprite_left[y][x] = sprite_right[y][x] = 0;
				x++;
				continue;
			}
			int end = x + 1;
			while (end < WIDTH && sprite_pixel(end, y))
				end++;
			for (int i = x; i < end; i++) {
				sprite_left[y][i] = (uint8_t)(i - x);
				sprite_right[y][i] = (uint8_t)(end - 1 - i);
			}
			x = end;
		}
	}
	/* One flat color reaching across half the screen is a field (a sky on a layer, a wall of
	 * one color), even where a bar or a letter cuts it into short columns. Runs of one color
	 * in a row join the runs of that color they touch in the row above. */
	static int seg_x0[WIDTH * MAX_HEIGHT], seg_x1[WIDTH * MAX_HEIGHT], seg_up[WIDTH * MAX_HEIGHT];
	static int row_seg[MAX_HEIGHT + 1];
	int segs = 0;
	for (int y = 0; y < height; y++) {
		row_seg[y] = segs;
		for (int x = 0; x < WIDTH;) {
			if (sprite_pixel(x, y)) {
				x++;
				continue;
			}
			int end = x + 1;
			while (end < WIDTH && grid[y][end] == grid[y][x] && !sprite_pixel(end, y))
				end++;
			seg_x0[segs] = x;
			seg_x1[segs] = end - 1;
			seg_up[segs] = segs;
			segs++;
			x = end;
		}
	}
	row_seg[height] = segs;
	for (int y = 1; y < height; y++) {
		int a = row_seg[y - 1];
		for (int i = row_seg[y]; i < row_seg[y + 1]; i++) {
			while (a < row_seg[y] && seg_x1[a] < seg_x0[i])
				a++;
			for (int j = a; j < row_seg[y] && seg_x0[j] <= seg_x1[i]; j++) {
				if (grid[y - 1][seg_x0[j]] != grid[y][seg_x0[i]])
					continue;
				int ri = i, rj = j;
				while (seg_up[ri] != ri)
					ri = seg_up[ri] = seg_up[seg_up[ri]];
				while (seg_up[rj] != rj)
					rj = seg_up[rj] = seg_up[seg_up[rj]];
				if (ri != rj)
					seg_up[ri > rj ? ri : rj] = ri < rj ? ri : rj;
			}
		}
	}
	static int root_x0[WIDTH * MAX_HEIGHT], root_x1[WIDTH * MAX_HEIGHT];
	for (int i = 0; i < segs; i++) {
		int r = i;
		while (seg_up[r] != r)
			r = seg_up[r];
		seg_up[i] = r;
		if (r == i) {
			root_x0[i] = seg_x0[i];
			root_x1[i] = seg_x1[i];
		} else {
			root_x0[r] = seg_x0[i] < root_x0[r] ? seg_x0[i] : root_x0[r];
			root_x1[r] = seg_x1[i] > root_x1[r] ? seg_x1[i] : root_x1[r];
		}
	}
	memset(field, 0, sizeof field);
	for (int y = 0; y < height; y++)
		for (int i = row_seg[y]; i < row_seg[y + 1]; i++) {
			int r = seg_up[i];
			if (root_x1[r] - root_x0[r] + 1 >= WIDTH / 2)
				for (int x = seg_x0[i]; x <= seg_x1[i]; x++)
					field[y][x] = true;
		}
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
			for (int i = 0; i < count; i++) {
				int pos = sprite_marks[i];
				int cy = pos / WIDTH;
				sprite_body[cy][pos - cy * WIDTH] = (uint8_t)body;
			}
		}
	}
	find_glyphs(height);
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

/* Positive when the pixel is sky-blue, negative when it is warm rock, grass, or skin. */
static int bias_of(uint32_t color)
{
	int r = (int)(color >> 16 & 255);
	int g = (int)(color >> 8 & 255);
	int b = (int)(color & 255);
	return b - (r > g ? r : g);
}

static bool stipple_at(int x, int y)
{
	int height = (int)src_height;
	if (y > 0 && y + 1 < height && grid[y - 1][x] == grid[y + 1][x] && grid[y - 1][x] != grid[y][x])
		return true;
	if (x > 0 && x + 1 < WIDTH && grid[y][x - 1] == grid[y][x + 1] && grid[y][x - 1] != grid[y][x])
		return true;
	return false;
}

/* Warm rock, grass, or skin. A bright cloud highlight stays above the luma cap,
 * so it is not treated as a body. */
static bool warm_body(uint32_t color)
{
	return bias_of(color) < -20 && luma_of(color) < 160;
}

static int warm_inward(int x, int y, int sign)
{
	int n = 0;
	for (int i = 0; i < 8; i++) {
		int xx = x - sign * i;
		if (xx < 0 || xx >= WIDTH || !warm_body(grid[y][xx]))
			break;
		n++;
	}
	return n;
}

/* `grow_down` fills the new column from the top. A shrinking edge fills from the bottom.
 * Over `len` console rows the cut moves one screen pixel at a time. */
static void cut_run(unsigned out_w, int x, int y, int len, int sign, bool grow_down)
{
	if (len < 1)
		return;
	for (int i = 0; i < len; i++) {
		int yy = y + i;
		if (yy < 0 || yy >= (int)src_height || x + sign < 0 || x + sign >= WIDTH)
			break;
		uint32_t color = stair_color[yy][x + sign];
		for (int sy = 0; sy < SCALE; sy++) {
			int along = i * SCALE + sy;
			int moved = along / len;
			if (moved > SCALE)
				moved = SCALE;
			int cut = grow_down ? SCALE - moved : moved;
			if (cut < 0)
				cut = 0;
			else if (cut > SCALE)
				cut = SCALE;
			uint32_t *row = &image[((size_t)yy * SCALE + (unsigned)sy) * out_w + (unsigned)x * SCALE];
			for (int k = 0; k < cut; k++) {
				int sx = sign > 0 ? SCALE - 1 - k : k;
				row[sx] = color;
			}
		}
	}
}

/* A step of two or three console pixels. The new cells stay put; the outside
 * color already beside the tip fills them so the edge moves a few screen pixels
 * per row instead of a ledge. A jump wider than that stays the block the game drew. */
static void cut_span(unsigned out_w, int x, int y, int n, int sign, bool grow_down)
{
	if (n < 1 || y < 0 || y >= (int)src_height)
		return;
	int tip = x + n * sign;
	if (tip < 0 || tip >= WIDTH)
		return;
	uint32_t color = stair_color[y][tip];
	int width = n * SCALE;
	for (int sy = 0; sy < SCALE; sy++) {
		int keep = grow_down ? sy * n : width - sy * n;
		if (keep < 0)
			keep = 0;
		else if (keep > width)
			keep = width;
		uint32_t *row = &image[((size_t)y * SCALE + (unsigned)sy) * out_w];
		for (int k = 0; k < n; k++) {
			int cx = x + k * sign;
			if (cx < 0 || cx >= WIDTH)
				continue;
			for (int sx = 0; sx < SCALE; sx++) {
				int from_in = sign > 0 ? k * SCALE + sx : k * SCALE + (SCALE - 1 - sx);
				if (from_in >= keep)
					row[cx * SCALE + sx] = color;
			}
		}
	}
}

/* A small cream shape sitting on the ground, such as a figure drawn into the
 * background. Cloud highlights are larger, or they sit against the sky, so the
 * cloud stays the original drawing. A square corner is not a step, so a tower
 * or a letter keeps the corner the game drew. */
static bool cream(uint32_t color)
{
	return bias_of(color) < -35 && luma_of(color) > 200;
}

static bool ink_pixel(uint32_t color)
{
	return luma_of(color) < 40;
}

static void cream_shapes(unsigned out_w)
{
	enum { MAX_COMP = 48, MAX_CELL = 48 };
	int height = (int)src_height;
	static uint8_t seen[MAX_HEIGHT][WIDTH];
	static uint8_t keep[MAX_HEIGHT][WIDTH];
	memset(seen, 0, sizeof seen);
	memset(keep, 0, sizeof keep);

	int cx[MAX_COMP][MAX_CELL], cy[MAX_COMP][MAX_CELL];
	int count[MAX_COMP], ink[MAX_COMP], sky_n[MAX_COMP], ground_n[MAX_COMP];
	int minx[MAX_COMP], maxx[MAX_COMP], miny[MAX_COMP], maxy[MAX_COMP];
	int ncomp = 0;

	for (int y = 0; y < height && ncomp < MAX_COMP; y++) {
		for (int x = 0; x < WIDTH && ncomp < MAX_COMP; x++) {
			if (seen[y][x] || !cream(grid[y][x]))
				continue;
			int sx[256], sy[256];
			int sp = 1;
			sx[0] = x;
			sy[0] = y;
			seen[y][x] = 1;
			int id = ncomp++;
			count[id] = ink[id] = sky_n[id] = ground_n[id] = 0;
			minx[id] = maxx[id] = x;
			miny[id] = maxy[id] = y;
			bool overflow = false;
			while (sp > 0) {
				sp--;
				int px = sx[sp], py = sy[sp];
				if (count[id] < MAX_CELL) {
					cx[id][count[id]] = px;
					cy[id][count[id]] = py;
				}
				count[id]++;
				if (px < minx[id])
					minx[id] = px;
				if (px > maxx[id])
					maxx[id] = px;
				if (py < miny[id])
					miny[id] = py;
				if (py > maxy[id])
					maxy[id] = py;
				static const int step[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (int k = 0; k < 4; k++) {
					int nx = px + step[k][0], ny = py + step[k][1];
					if (nx < 0 || ny < 0 || nx >= WIDTH || ny >= height)
						continue;
					if (cream(grid[ny][nx])) {
						if (seen[ny][nx])
							continue;
						seen[ny][nx] = 1;
						if (sp >= 256) {
							overflow = true;
							continue;
						}
						sx[sp] = nx;
						sy[sp] = ny;
						sp++;
					} else {
						uint32_t other = grid[ny][nx];
						int b = bias_of(other);
						if (luma_of(other) < 40)
							ink[id]++;
						if (b > 12)
							sky_n[id]++;
						else if (b < -20 && luma_of(other) < 140)
							ground_n[id]++;
					}
				}
			}
			if (overflow)
				count[id] = MAX_CELL + 1;
		}
	}

	for (int id = 0; id < ncomp; id++) {
		if (count[id] > 16 || count[id] < 1)
			continue;
		if (maxx[id] - minx[id] < 1 || maxy[id] - miny[id] < 1)
			continue;
		if (ground_n[id] < 4 || ground_n[id] <= sky_n[id])
			continue;
		bool outlined = ink[id] >= 2;
		if (!outlined) {
			for (int other = 0; other < ncomp && !outlined; other++) {
				if (ink[other] < 2)
					continue;
				int stored = count[other] < MAX_CELL ? count[other] : MAX_CELL;
				for (int a = 0; a < count[id] && !outlined; a++) {
					for (int b = 0; b < stored; b++) {
						int dx = cx[id][a] - cx[other][b];
						int dy = cy[id][a] - cy[other][b];
						if (dx < 0)
							dx = -dx;
						if (dy < 0)
							dy = -dy;
						if ((dx > dy ? dx : dy) <= 2) {
							outlined = true;
							break;
						}
					}
				}
			}
		}
		if (!outlined)
			continue;
		for (int i = 0; i < count[id]; i++)
			keep[cy[id][i]][cx[id][i]] = 1;
	}

	for (int y = 1; y < height - 2; y++) {
		for (int x = 1; x < WIDTH - 1; x++) {
			if (!keep[y][x] || stipple_at(x, y) || cream(grid[y - 1][x]))
				continue;
			for (int sign = 1; sign >= -1; sign -= 2) {
				int xo = x + sign;
				int xi = x - sign;
				if (xo < 0 || xo >= WIDTH || xi < 0 || xi >= WIDTH || cream(grid[y][xo]))
					continue;
				/* The column already steps. A flat corner of a rectangle does not. */
				bool growing = cream(grid[y + 1][x]) && cream(grid[y + 1][xo]) && cream(grid[y + 2][xo]);
				bool ending = !cream(grid[y + 1][x]) && cream(grid[y][xi]) && cream(grid[y - 1][xi]);
				if (growing || ending)
					cut_run(out_w, x, y, 1, sign, true);
			}
		}
	}

	/* The dark of that same small shape, such as the pants beside the shirt.
	 * It has to sit next to the cream we already kept, so a window or a letter
	 * is not a figure. Only a column that already steps is cut. */
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (seen[y][x] || !ink_pixel(grid[y][x]))
				continue;
			int sx[64], sy[64], cx[16], cy[16];
			int sp = 1, n = 0;
			sx[0] = x;
			sy[0] = y;
			seen[y][x] = 1;
			int minx = x, maxx = x, miny = y, maxy = y;
			bool overflow = false;
			while (sp > 0) {
				sp--;
				int px = sx[sp], py = sy[sp];
				if (n < 16) {
					cx[n] = px;
					cy[n] = py;
				}
				n++;
				if (px < minx)
					minx = px;
				if (px > maxx)
					maxx = px;
				if (py < miny)
					miny = py;
				if (py > maxy)
					maxy = py;
				static const int step[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (int k = 0; k < 4; k++) {
					int nx = px + step[k][0], ny = py + step[k][1];
					if (nx < 0 || ny < 0 || nx >= WIDTH || ny >= height || seen[ny][nx])
						continue;
					if (!ink_pixel(grid[ny][nx]))
						continue;
					seen[ny][nx] = 1;
					if (sp >= 64) {
						overflow = true;
						continue;
					}
					sx[sp] = nx;
					sy[sp] = ny;
					sp++;
				}
			}
			if (overflow || n < 2 || n > 16 || maxx - minx < 1 || maxy - miny < 1)
				continue;
			bool near = false;
			for (int i = 0; i < n && !near; i++) {
				for (int dy = -2; dy <= 2 && !near; dy++) {
					for (int dx = -2; dx <= 2; dx++) {
						int ny = cy[i] + dy, nx = cx[i] + dx;
						if (ny >= 0 && nx >= 0 && ny < height && nx < WIDTH && keep[ny][nx]) {
							near = true;
							break;
						}
					}
				}
			}
			if (!near)
				continue;
			for (int i = 0; i < n; i++) {
				int px = cx[i], py = cy[i];
				if (py < 1 || py + 1 >= height || px < 2 || px >= WIDTH - 2 || stipple_at(px, py))
					continue;
				for (int sign = 1; sign >= -1; sign -= 2) {
					int xo = px + sign, xi = px - sign;
					if (ink_pixel(grid[py][xo]) || cream(grid[py][xo]) || bias_of(grid[py][xo]) > 12)
						continue;
					bool shrinking = !ink_pixel(grid[py + 1][px]) && ink_pixel(grid[py][xi])
						&& ink_pixel(grid[py + 1][xi]) && !ink_pixel(grid[py + 1][xo]);
					bool growing = !ink_pixel(grid[py - 1][px]) && ink_pixel(grid[py + 1][px])
						&& ink_pixel(grid[py + 1][xo]) && ink_pixel(grid[py][xi]);
					if (shrinking)
						cut_run(out_w, px, py, 1, sign, false);
					else if (growing)
						cut_run(out_w, px, py, 1, sign, true);
				}
			}
		}
	}
}

/* Two pixels of background, and a one-pixel spur does not count as a wall.
 * A two-pixel crack between parts of the ship stays a square corner. */
static bool open_outside(int x, int y, int sign)
{
	int xo = x + sign;
	int xoo = x + 2 * sign;
	int far = x + 3 * sign;
	if (sprite_pixel(xo, y) || sprite_pixel(xoo, y))
		return false;
	if (!sprite_pixel(far, y))
		return true;
	return !sprite_pixel(far + sign, y);
}

/* Where the silhouette ends on row y, in pixels outward from x (sign is outward), or
 * NO_EDGE when the figure does not reach that row near x. */
#define NO_EDGE 99
static int edge_at(int x, int y, int sign)
{
	int from = x - 4 * sign;
	if (y < 0 || y >= (int)src_height || from < 0 || from >= WIDTH || !sprite_pixel(from, y))
		return NO_EDGE;
	int at = -4;
	while (at < 4 && sprite_pixel(x + (at + 1) * sign, y))
		at++;
	return at;
}

/* A stair belongs on a slope the artist drew: the outline keeps moving one way over the rows
 * around it. A photographed or digitized figure has an outline that wobbles in and out a pixel
 * at a time; a stair there is a thorn. */
static bool steady_outline(int x, int y, int sign)
{
	int last = NO_EDGE, in = 0, out = 0;
	for (int k = -2; k <= 2; k++) {
		int e = edge_at(x, y + k, sign);
		if (e == NO_EDGE)
			continue;
		if (last != NO_EDGE) {
			in += e < last;
			out += e > last;
		}
		last = e;
	}
	return !(in && out);
}

/* The silhouette runs straight down (dir 1) or up (dir -1) from x for two more rows. A stair
 * there is a notch at the top of a wall, and every such wall on a steep slope makes a saw. */
static bool wall_from(int x, int y, int dir, int sign)
{
	return edge_at(x, y + dir, sign) == 0 && edge_at(x, y + 2 * dir, sign) == 0;
}

typedef struct {
	unsigned out_w, out_h;
	int height;
} Canvas;

/* A cut writes only into its own console row and takes its colors from before any cut, so the
 * rows step at once. */
static void stair_rows(void *context, int y0, int y1)
{
	unsigned out_w = ((const Canvas *)context)->out_w;
	int height = (int)src_height;
	for (int y = y0 < 1 ? 1 : y0; y < y1 && y < height - 1; y++) {
		for (int x = 2; x < WIDTH - 2; x++) {
			if (glyph[y][x])
				continue;
			/* A checker dot matches on both axes and stays drawn. A warm edge against
			 * a flat field matches on one axis only; that is a step, not stipple. */
			bool checker = false;
			if (stipple_at(x, y)) {
				bool vert = y > 0 && y + 1 < height && grid[y - 1][x] == grid[y + 1][x]
					&& grid[y - 1][x] != grid[y][x];
				bool horiz = grid[y][x - 1] == grid[y][x + 1] && grid[y][x - 1] != grid[y][x];
				checker = vert && horiz;
				/* A checker stays drawn. A one-axis match on a character is the
				 * silhouette between two sky pixels, not a cloud dot. */
				bool character = sprite_pixel(x, y) && sprite_body[y][x] >= 14;
				if (checker || (!warm_body(grid[y][x]) && !character))
					continue;
			}
			bool sprite = sprite_pixel(x, y);
			if (sprite) {
				int span_h = (int)sprite_left[y][x] + sprite_right[y][x] + 1;
				/* The whole shape is the test. A copyright letter is about eight
				 * pixels tall. A hair column of a character is short, but the
				 * character is not, so that column may still step. */
				if (sprite_body[y][x] < 14 || span_h < 2)
					continue;
			}
			for (int sign = 1; sign >= -1; sign -= 2) {
				int xo = x + sign;
				int xi = x - sign;
				int xoo = x + 2 * sign;
				int xii = xi - sign;
				uint32_t here = grid[y][x];
				uint32_t out = grid[y][xo];
				int gap = luma_of(here) - luma_of(out);
				if (gap < 0)
					gap = -gap;
				/* The outer silhouette of a warm shape. Two pixels outside are not
				 * body, so a one-pixel hole stays put. A two-pixel tip stairs only
				 * when the row above already reached the inward pixel: that is a
				 * one-pixel step, not the flat top of a thin rock. */
				int depth = warm_inward(x, y, sign);
				bool warm_edge = !sprite && warm_body(here) && warm_body(grid[y][xi])
					&& !warm_body(out) && !warm_body(grid[y][xoo]) && !sprite_pixel(xo, y) && !sprite_pixel(xoo, y);
				bool thick = depth >= 3;
				bool tip = depth == 2 && warm_body(grid[y - 1][xi]);
				if (warm_edge && (thick || tip) && !warm_body(grid[y - 1][x])) {
					/* A step of two or three shares one stair. The outer cell still
					 * steps when the body behind it does not. */
					int n = 0;
					int inner = x;
					while (n < 3) {
						int xx = x - n * sign;
						if (xx < 0 || xx >= WIDTH || !warm_body(grid[y][xx])
							|| warm_body(grid[y - 1][xx]))
							break;
						inner = xx;
						n++;
					}
					int behind = inner - sign;
					if (n >= 2 && n <= 3 && behind >= 0 && behind < WIDTH
						&& warm_body(grid[y][behind]) && warm_body(grid[y - 1][behind]))
						cut_span(out_w, inner, y, n, sign, true);
					else
						cut_run(out_w, x, y, 1, sign, true);
					continue;
				}
				/* The next row steps inward. A step of one pixel ramps this cell.
				 * A step of two or three shares that ramp. The side above stays straight. */
				if (warm_edge && depth >= 2 && warm_body(grid[y - 1][x])
					&& !warm_body(grid[y + 1][x])) {
					int n = 0;
					int inner = x;
					while (n < 3) {
						int xx = x - n * sign;
						if (xx < 0 || xx >= WIDTH || !warm_body(grid[y][xx])
							|| warm_body(grid[y + 1][xx]))
							break;
						inner = xx;
						n++;
					}
					int behind = inner - sign;
					if (n >= 1 && n <= 3 && behind >= 0 && behind < WIDTH
						&& warm_body(grid[y][behind]) && warm_body(grid[y + 1][behind]))
						cut_span(out_w, inner, y, n, sign, false);
					continue;
				}
				/* The sprite silhouette is already an edge. A brown hull against a gray
				 * wall can sit under the luma gap and still be a different color.
				 * Background steps still need a real gap, so texture is left alone. */
				if (sprite) {
					if (here == out)
						continue;
				} else if (gap < 40 || sprite_pixel(xo, y) || sprite_pixel(xoo, y)) {
					continue; /* a figure in front is not the field the scenery steps against */
				}

				bool band = sprite && sprite_pixel(xo, y) && sprite_pixel(xi, y)
					&& xi >= 0 && xi < WIDTH && grid[y][xi] == here
					&& out != here && xoo >= 0 && xoo < WIDTH && grid[y][xoo] == out
					&& gap >= 24;
				bool thick3 = band && xii >= 0 && xii < WIDTH && grid[y][xii] == here;
				if (thick3) {
					/* A thick band inside the ship, not a one-pixel stroke. The corner
					 * copies the flat color already beside it. */
					if (grid[y - 1][x] != here) {
						int n = 0;
						int inner = x;
						while (n < 3) {
							int xx = x - n * sign;
							if (xx < 0 || xx >= WIDTH || grid[y][xx] != here
								|| grid[y - 1][xx] == here)
								break;
							inner = xx;
							n++;
						}
						int behind = inner - sign;
						if (n >= 1 && n <= 3 && behind >= 0 && behind < WIDTH
							&& grid[y][behind] == here && grid[y - 1][behind] == here) {
							cut_span(out_w, inner, y, n, sign, true);
							continue;
						}
						/* A long band has no same color above it. Only the outer cell
						 * steps; the rest of the band stays the block the game drew. */
						if (n == 3) {
							int fourth = x - 3 * sign;
							if (fourth >= 0 && fourth < WIDTH
								&& grid[y][fourth] == here && grid[y - 1][fourth] != here) {
								cut_span(out_w, x, y, 1, sign, true);
								continue;
							}
						}
					} else if (grid[y + 1][x] != here) {
						int n = 0;
						int inner = x;
						while (n < 3) {
							int xx = x - n * sign;
							if (xx < 0 || xx >= WIDTH || grid[y][xx] != here
								|| grid[y + 1][xx] == here)
								break;
							inner = xx;
							n++;
						}
						int behind = inner - sign;
						if (n >= 1 && n <= 3 && behind >= 0 && behind < WIDTH
							&& grid[y][behind] == here && grid[y + 1][behind] == here) {
							cut_span(out_w, inner, y, n, sign, false);
							continue;
						}
						if (n == 3) {
							int fourth = x - 3 * sign;
							if (fourth >= 0 && fourth < WIDTH
								&& grid[y][fourth] == here && grid[y + 1][fourth] != here) {
								cut_span(out_w, x, y, 1, sign, false);
								continue;
							}
						}
					}
				} else if (band) {
					/* Two pixels of the same color. Only a one-pixel step, and only
					 * when the pixel inward already had that color. A one-pixel
					 * stroke never reaches here. */
					if (grid[y - 1][x] != here && grid[y - 1][xi] == here)
						cut_run(out_w, x, y, 1, sign, true);
					else if (grid[y - 1][x] == here && grid[y + 1][x] != here
						&& grid[y + 1][xi] == here)
						cut_run(out_w, x, y, 1, sign, false);
				}
				if (sprite) {
					/* Only the outer pixel of the silhouette. A step of one pixel is
					 * the old stair. A step of two or three shares one stair across
					 * those cells, so the airship curve is not a ledge. A wider jump
					 * keeps the cells the game drew, and only its outer corner steps,
					 * when the next row keeps moving and the outside is open. */
					if (sprite_pixel(xo, y) || !sprite_pixel(xi, y) || !steady_outline(x, y, sign))
						continue;
					bool stepped = false;
					if (!sprite_pixel(x, y - 1) && !wall_from(x, y, 1, sign)) {
						int n = 0;
						int inner = x;
						while (n < 3) {
							int xx = x - n * sign;
							if (xx < 0 || xx >= WIDTH || !sprite_pixel(xx, y) || sprite_pixel(xx, y - 1))
								break;
							inner = xx;
							n++;
						}
						int behind = inner - sign;
						if (n >= 1 && n <= 3 && behind >= 0 && behind < WIDTH
							&& sprite_pixel(behind, y) && sprite_pixel(behind, y - 1)
							&& (n > 1 || sprite_pixel(behind - sign, y))
							&& !sprite_pixel(xo, y)) {
							cut_span(out_w, inner, y, n, sign, true);
							stepped = true;
						} else if (n == 3) {
							int fourth = x - 3 * sign;
							bool longer = fourth >= 0 && fourth < WIDTH
								&& sprite_pixel(fourth, y) && !sprite_pixel(fourth, y - 1);
							if (longer && sprite_pixel(x, y + 1) && open_outside(x, y, sign)) {
								cut_span(out_w, x, y, 1, sign, true);
								stepped = true;
							}
						}
					}
					if (!stepped && !sprite_pixel(x, y + 1) && !wall_from(x, y, -1, sign)) {
						int n = 0;
						int inner = x;
						while (n < 3) {
							int xx = x - n * sign;
							if (xx < 0 || xx >= WIDTH || !sprite_pixel(xx, y) || sprite_pixel(xx, y + 1))
								break;
							inner = xx;
							n++;
						}
						int behind = inner - sign;
						if (n >= 1 && n <= 3 && behind >= 0 && behind < WIDTH
							&& sprite_pixel(behind, y + 1)
							&& (n > 1 || sprite_pixel(behind - sign, y))
							&& !sprite_pixel(xo, y))
							cut_span(out_w, inner, y, n, sign, false);
						else if (n == 3) {
							int fourth = x - 3 * sign;
							bool longer = fourth >= 0 && fourth < WIDTH
								&& sprite_pixel(fourth, y) && !sprite_pixel(fourth, y + 1);
							if (longer && sprite_pixel(x, y - 1) && open_outside(x, y, sign))
								cut_span(out_w, x, y, 1, sign, false);
						}
					}
					continue;
				} else {
					/* A thick edge against a flat black, a flat bright field, or the
					 * other way around: a bright shape against a darker field.
					 * Same color three pixels in, and the row above already
					 * stepped, so a square window stays square. Sky against sky
					 * stays, and so does the cloud band. */
					int field = luma_of(out);
					int body = luma_of(here);
					bool blue_pair = bias_of(here) > 20 && bias_of(out) > 20;
					if (y >= 45 && !blue_pair && gap >= 40
						&& xii >= 0 && xii < WIDTH
						&& grid[y][xi] == here && grid[y][xii] == here
						&& xoo >= 0 && xoo < WIDTH && grid[y][xoo] == out
						&& grid[y - 1][x] != here
						&& grid[y - 1][xo] == out
						&& !sprite_pixel(x, y - 1)
						&& (field < 12 || (field >= 150 && body <= 90)
							|| (body >= 150 && field <= 90 && field >= 12))) {
						int n = 0;
						int inner = x;
						while (n < 3) {
							int xx = x - n * sign;
							if (xx < 0 || xx >= WIDTH || grid[y][xx] != here
								|| grid[y - 1][xx] == here)
								break;
							inner = xx;
							n++;
						}
						int behind = inner - sign;
						bool bright_on_dark = body >= 150 && field <= 90 && field >= 12;
						if (n >= 2 && n <= 3 && body > 16 && !bright_on_dark
							&& behind >= 0 && behind < WIDTH
							&& grid[y][behind] == here && grid[y - 1][behind] == here)
							cut_span(out_w, inner, y, n, sign, true);
						else if (n == 1 && grid[y - 1][xi] == here)
							cut_run(out_w, x, y, 1, sign, true);
						else
							goto field_done;
						continue;
					}
					if (y >= 45 && !blue_pair && gap >= 40
						&& xi >= 0 && xi < WIDTH && grid[y][xi] == here
						&& (xii < 0 || xii >= WIDTH || grid[y][xii] != here)
						&& xoo >= 0 && xoo < WIDTH && grid[y][xoo] == out
						&& grid[y - 1][x] != here
						&& grid[y - 1][xo] == out
						&& grid[y - 1][xi] == here
						&& !sprite_pixel(x, y - 1)
						&& (field < 12 || (field >= 150 && body <= 90)
							|| (body >= 150 && field <= 90 && field >= 12))) {
						/* Two pixels of the same color. Only this outer cell steps. */
						cut_run(out_w, x, y, 1, sign, true);
						continue;
					}
					field_done:;
					/* The color beside the step has to be a flat region, not a one-pixel band. */
					int side = luma_of(out) - luma_of(grid[y][xoo]);
					if (side < 0)
						side = -side;
					if (side > 22)
						continue;
					int above = luma_of(grid[y - 1][x]) - luma_of(out);
					if (above < 0)
						above = -above;
					if (above > 32)
						continue;
					int above_here = luma_of(grid[y - 1][x]) - luma_of(here);
					if (above_here < 0)
						above_here = -above_here;
					if (above_here < 24)
						continue; /* the column already existed: a vertical edge */
					int in_l = luma_of(grid[y][xi]) - luma_of(out);
					if (in_l < 0)
						in_l = -in_l;
					if (in_l < 30)
						continue;
					int in_l2 = luma_of(grid[y][xii]) - luma_of(out);
					if (in_l2 < 0)
						in_l2 = -in_l2;
					if (in_l2 < 30)
						continue;
					/* Above and inward has to be the shape. A flat top corner stays square. */
					int back = luma_of(grid[y - 1][xi]) - luma_of(out);
					if (back < 0)
						back = -back;
					if (back < 30)
						continue;
					/* Land against a flat sky, or against a flat black field such as a cave.
					 * A cloud highlight is not warm, so the cloud stays drawn. */
					if (bias_of(out) <= 12 && luma_of(out) >= 28)
						continue;
					int warm = 99;
					for (int i = 0; i < 5; i++) {
						int xx = x - sign * i;
						if (xx < 0 || xx >= WIDTH)
							break;
						int b = bias_of(grid[y][xx]);
						if (b < warm)
							warm = b;
					}
					if (warm > -15)
						continue;
				}

				cut_run(out_w, x, y, 1, sign, true);
			}
		}
	}
}

static void stairs(unsigned out_w)
{
	int height = (int)src_height;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH; x++)
			stair_color[y][x] = image[(size_t)y * SCALE * out_w + (size_t)x * SCALE];
	}
	Canvas canvas = { out_w, (unsigned)height * SCALE, height };
	remaster_bands(height, stair_rows, &canvas);
	cream_shapes(out_w);
}

static void paint_color(unsigned out_w, unsigned out_h, int x, int y, uint32_t color, int from, int to)
{
	for (int j = from; j < to && j < SCALE; j++) {
		unsigned py = (unsigned)y * SCALE + (unsigned)j;
		if (py >= out_h)
			continue;
		uint32_t *dst = &image[(size_t)py * out_w + (unsigned)x * SCALE];
		for (int i = 0; i < SCALE; i++)
			dst[i] = color;
	}
}

#define MAX_SHAPE_RUN 96

/* Two neighbors in a smooth blend: the same tone a small step apart. Pixel art shades in
 * bigger steps; a painted or digitized picture blends in small ones. */
static bool blend_step(uint32_t a, uint32_t b)
{
	if (a == b)
		return false;
	int dr = (int)(a >> 16 & 255) - (int)(b >> 16 & 255);
	int dg = (int)(a >> 8 & 255) - (int)(b >> 8 & 255);
	int db = (int)(a & 255) - (int)(b & 255);
	int dl = luma_of(a) - luma_of(b);
	return abs(dr) <= 40 && abs(dg) <= 40 && abs(db) <= 40 && abs(dl) <= 28;
}

/* Hue in degrees, or -1 for a gray. */
static int hue_of(uint32_t c)
{
	int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
	int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	int span = hi - lo;
	if (span < 24)
		return -1;
	int h = hi == r ? 60 * (g - b) / span : hi == g ? 120 + 60 * (b - r) / span : 240 + 60 * (r - g) / span;
	return h < 0 ? h + 360 : h;
}

static bool same_hue(uint32_t a, uint32_t b)
{
	int ha = hue_of(a), hb = hue_of(b);
	if (ha < 0 || hb < 0)
		return ha == hb;
	int d = abs(ha - hb);
	return (d > 180 ? 360 - d : d) <= 24;
}

/* A step of a painted ramp: one hue getting lighter (or darker) through the color above, this
 * one and the one below, as a sky or a lava glow is painted in bands. */
static bool ramp_step(uint32_t above, uint32_t here, uint32_t below)
{
	int a = luma_of(above), h = luma_of(here), b = luma_of(below);
	return ((a < h && h < b) || (a > h && h > b)) && same_hue(above, here) && same_hue(here, below);
}

/* A run of one color down a column that the game drew as a flat shape: not stipple, not a
 * stroke, not a warm body, not a painted blend, not cut by the screen or by a figure in front. */
static bool flat_shape(int x, int y, int height)
{
	uint32_t here = grid[y][x];
	if (glyph[y][x] || sprite_pixel(x, y))
		return false;
	if (y > 0 && y + 1 < height && grid[y - 1][x] == grid[y + 1][x] && grid[y - 1][x] != here)
		return false; /* stipple, such as a cloud */
	if (wide_run[y][x] < 3)
		return false; /* a one-pixel stroke, such as a letter */
	if (luma_of(here) >= 230)
		return false; /* white is a glow or a flash; a step on it reads as a gray mark */
	/* A tall warm body (a cliff, a field, a lava fall) is many colors over many rows,
	 * and steps lit by hue land on a painted blend as stripes. It keeps its colors. */
	if (warm_body(here) && (int)warm_above[y][x] + warm_below[y][x] + 1 >= 14)
		return false;
	int len = tall_run[y][x];
	int pos = run_at[y][x];
	if (len < 3)
		return false;
	/* A band of a painted or digitized blend (its neighbor above or below is the next shade
	 * of it) is already lit, and the backdrop or a run as tall as a sky is not a shape. */
	if (len > MAX_SHAPE_RUN || key_at(src_motion, y + src_motion->top, x) == 6
		|| y - pos == 0 || y - pos + len >= height)
		return false;
	if (field[y][x])
		return false;
	if (blend_step(here, grid[y - pos - 1][x]) || blend_step(here, grid[y - pos + len][x])
		|| ramp_step(grid[y - pos - 1][x], here, grid[y - pos + len][x]))
		return false;
	return !sprite_pixel(x, y - pos - 1) && !sprite_pixel(x, y - pos + len);
}

static void bevel_rows(void *context, int y0, int y1)
{
	const Canvas *canvas = context;
	unsigned out_w = canvas->out_w, out_h = canvas->out_h;
	int height = canvas->height;
	for (int y = y0; y < y1; y++) {
		/* The step of each flat pixel of this row: 0 none, 124 or 112 lit, 62 or 72 shaded. */
		int step[WIDTH];
		for (int x = 0; x < WIDTH; x++) {
			uint32_t here = grid[y][x];
			step[x] = 0;
			if (glyph[y][x])
				continue;
			if (sprite_pixel(x, y)) {
				/* The body, measured on the silhouette. Color runs of one pixel
				 * are the hair and the cloth, not a reason to skip the character.
				 * A mark one pixel wide, such as a letter, stays drawn. */
				int above = sprite_above[y][x];
				int below = sprite_below[y][x];
				int span_v = above + below + 1;
				int span_h = (int)sprite_left[y][x] + sprite_right[y][x] + 1;
				if (span_v < 14 || span_h < 2)
					continue; /* a letter stays the drawn color */
				/* A third of the body, in two hard steps. The middle stays drawn. */
				int band = span_v / 3;
				if (band < 1)
					band = 1;
				if (band * 2 >= span_v)
					band = (span_v - 1) / 2;
				int half = band / 2;
				/* The outline stays the drawn pixel. A light on that stair is what
				 * reads as a serration; the step sits on the row inside it. */
				if (above == 0 || below == 0 || sprite_left[y][x] == 0 || sprite_right[y][x] == 0)
					continue;
				/* Same hue. Mixing in a beige would turn a black pixel beige and scatter it over the picture. */
				if (above < band && below >= band)
					paint_color(out_w, out_h, x, y, shade(here, half > 0 && above >= half ? 112 : 124), 0, SCALE);
				else if (below < band && above >= band)
					paint_color(out_w, out_h, x, y, shade(here, half > 0 && below >= half ? 78 : 62), 0, SCALE);
				continue;
			}
			if (!flat_shape(x, y, height))
				continue;
			/* A column that passes beside one of the same color that does not is a stripe,
			 * not the lit side of a shape. */
			if ((x > 0 && grid[y][x - 1] == here && !flat_shape(x - 1, y, height))
				|| (x + 1 < WIDTH && grid[y][x + 1] == here && !flat_shape(x + 1, y, height)))
				continue;
			/* Light from above, on the inside of a tall shape. The first and last
			 * pixel of the run are the outline, and they stay drawn. */
			int len = tall_run[y][x];
			int pos = run_at[y][x];
			int band = len / 3;
			if (pos == 0 || pos == len - 1)
				continue;
			/* The edge the step follows moves at most a pixel to the next column, so the step
			 * is a contour and not a notch in a flat field. */
			bool ragged = false;
			for (int nx = x - 1; nx <= x + 1; nx += 2) {
				if (nx < 0 || nx >= WIDTH || grid[y][nx] != here)
					continue;
				int top_d = run_at[y][nx] - pos;
				int bottom_d = (tall_run[y][nx] - run_at[y][nx]) - (len - pos);
				if ((pos < band && abs(top_d) > 1) || (pos >= band && abs(bottom_d) > 1))
					ragged = true;
			}
			if (ragged)
				continue;
			/* Two flat steps on each side, same hue. The step nearer the outline
			 * is the stronger one. A short band keeps the single step it had. */
			int half = band / 2;
			int from_bottom = len - 1 - pos;
			if (pos < band)
				step[x] = half > 0 && pos < half ? 124 : 112;
			else if (from_bottom < band)
				step[x] = half > 0 && from_bottom < half ? 62 : 72;
		}
		/* A step one column wide is a streak down a flat field, not the side of a shape. */
		for (int x = 0; x < WIDTH; x++)
			if (step[x] && ((x > 0 && step[x - 1] == step[x]) || (x + 1 < WIDTH && step[x + 1] == step[x])))
				paint_color(out_w, out_h, x, y, shade(grid[y][x], step[x]), 0, SCALE);
	}
}

static void bevel(unsigned out_w, unsigned out_h)
{
	prepare_grid();
	Canvas canvas = { out_w, out_h, (int)src_height };
	remaster_bands(canvas.height, bevel_rows, &canvas);
}

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
	char kind;  /* 'f' flame, 'g' glow, 'w' window, 'n' none */
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
			: !strcmp(kind, "none") ? 'n' : 0;
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
static void wide_pass(const CoreMotion *m, const Light *lights, int count, int h);

static void emit_rows(void *context, int y0, int y1)
{
	const CoreMotion *m = context;
	for (int y = y0; y < y1; y++)
		for (int x = 0; x < WIDTH; x++)
			emits[y][x] = (uint8_t)emitter(m, y, x);
}

static void light_pass(const CoreMotion *m, unsigned height, unsigned out_w)
{
	int h = (int)height;
	find_room(m, h);
	if (detect_lights)
		remaster_bands(h, emit_rows, (void *)m);
	else
		memset(emits, 0, sizeof emits);
	/* The game's own list overrides the detection, a run of one tile at a time. listed_at and
	 * tile_keys are only read where a pixel emits. */
	if (listed_count > 0 || finding_lights) {
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
					if (finding_lights)
						tile_keys[y][x] = key;
					if (at < 0)
						continue;
					listed_at[y][x] = (int16_t)at;
					emits[y][x] = listed[at].kind != 'n' && m->brightness[row] >= 15
						&& luma_of(m->color[row][x] & 0xffffff) >= listed[at].luma ? 9 : 0;
				}
			}
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
		int radius = l->radius * (l->flame ? 94 + (int)(light_hash(m->frame / 5 + (unsigned)i) % 7) : 100) / 100;
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
				int ring = (int)(a * 4.0f + 0.75f); /* flat rings: 0..4 */
				if (ring <= 0)
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
	remaster_bands(h, apply_rows, &canvas);
}

/* Wide lights: from every glowing pixel of them at once, outward, each pixel learns the nearest
 * one and how far it is. Flat rings by that distance on the scenery; on a figure, the edge that
 * faces the glow takes its color, the way lava or a bright window rims what stands before it. */
#define WIDE_REACH 40
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
			int ring = (int)((1.0f - d / (float)WIDE_REACH) * 4.0f + 0.75f);
			if (ring <= 0)
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
					k = 40 + ring * 24; /* the edge that faces the glow */
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

static void apply_rows(void *context, int y0, int y1)
{
	unsigned out_w = ((const Canvas *)context)->out_w;
	for (int y = y0; y < y1; y++) {
		for (int x = 0; x < WIDTH; x++) {
			int gr = gain[y][x][0], gg = gain[y][x][1], gb = gain[y][x][2];
			int ar = glow[y][x][0], ag = glow[y][x][1], ab = glow[y][x][2];
			if (gr == 256 && gg == 256 && gb == 256 && (ar | ag | ab) == 0)
				continue;
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
			for (int sy = 0; sy < SCALE; sy++) {
				uint32_t *p = &image[((size_t)y * SCALE + (unsigned)sy) * out_w + (size_t)x * SCALE];
				for (int sx = 0; sx < SCALE; sx++) {
					uint32_t c = p[sx];
					int r = ((int)(c >> 16 & 255) * g3[0] >> 8) + ar;
					int g = ((int)(c >> 8 & 255) * g3[1] >> 8) + ag;
					int b = ((int)(c & 255) * g3[2] >> 8) + ab;
					if (r > 255)
						r = 255;
					if (g > 255)
						g = 255;
					if (b > 255)
						b = 255;
					p[sx] = (c & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
				}
			}
		}
	}
}

static void fill_rows(void *context, int y0, int y1)
{
	unsigned out_w = ((const Canvas *)context)->out_w;
	for (int y = y0; y < y1; y++) {
		for (int x = 0; x < WIDTH; x++) {
			uint32_t color = at_xy(x, y);
			uint32_t *dst = &image[((size_t)y * SCALE) * out_w + (size_t)x * SCALE];
			for (int j = 0; j < SCALE; j++)
				for (int i = 0; i < SCALE; i++)
					dst[(size_t)j * out_w + (unsigned)i] = color;
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

	Canvas canvas = { out_w, out_h, (int)height };
	remaster_bands((int)height, fill_rows, &canvas);

	bevel(out_w, out_h);
	stairs(out_w);

	memset(shadowed, 0, (size_t)out_w * out_h);
	/* The cutout: a figure stands in front of the world, so it shows a side face
	 * toward the lower right in its own color, one step darker, and casts a flat
	 * shadow one console pixel past that face. The figure's own cell keeps every
	 * pixel it drew; only pixels outside the figure change. A letter is too short
	 * to be a cutout, and the shadow never lands on the sky or another figure. */
	int face = SCALE / 2;
	if (face < 1)
		face = 1;
	int reach = face + SCALE;
	for (unsigned y = 0; y < height; y++) {
		for (int x = 0; x < WIDTH; x++) {
			if (sprite_body[y][x] < 14 || !sprite_pixel(x, y))
				continue;
			uint32_t drawn = stair_color[y][x];
			for (int sy = 0; sy < SCALE; sy++) {
				size_t py = (size_t)y * SCALE + (unsigned)sy;
				for (int sx = 0; sx < SCALE; sx++) {
					size_t px = (size_t)x * SCALE + (unsigned)sx;
					uint32_t c = image[py * out_w + px];
					if (c != drawn)
						continue;
					if (py + 1 < out_h && px + 1 < out_w) {
						int fy = (int)((py + 1) / (size_t)SCALE), fx = (int)((px + 1) / (size_t)SCALE);
						size_t first = (py + 1) * out_w + px + 1;
						if (shadowed[first] == 0 && image[first] == stair_color[fy][fx] && sprite_pixel(fx, fy))
							continue; /* the figure goes on down-right: nothing shows past this pixel */
					}
					uint32_t body = c;
					if (luma_of(body) <= 24) {
						for (int in = 1; in <= 2 * SCALE; in++) {
							if (py < (size_t)in || px < (size_t)in)
								break;
							size_t iy = py - (size_t)in, ix = px - (size_t)in;
							int cy = (int)(iy / (size_t)SCALE), cx = (int)(ix / (size_t)SCALE);
							if (!sprite_pixel(cx, cy))
								break;
							uint32_t ic = image[iy * out_w + ix];
							if (ic != stair_color[cy][cx])
								break;
							if (luma_of(ic) > 24) {
								body = ic;
								break;
							}
						}
					}
					uint32_t side = shade(body, 58);
					for (int k = 1; k <= reach; k++) {
						size_t ty = py + (size_t)k, tx = px + (size_t)k;
						if (ty >= out_h || tx >= out_w)
							break;
						int cy = (int)(ty / (size_t)SCALE), cx = (int)(tx / (size_t)SCALE);
						size_t at = ty * out_w + tx;
						bool figure = sprite_pixel(cx, cy);
						if (figure && image[at] == stair_color[cy][cx] && shadowed[at] == 0)
							break;
						uint8_t prev = shadowed[at];
						if (prev && prev <= k)
							continue;
						if (k <= face) {
							image[at] = side;
							shadowed[at] = (uint8_t)k;
							continue;
						}
						if (figure)
							continue;
						int ground = key_at(m, cy + m->top, cx);
						if (ground < 0 || ground > 3 || emitter(m, cy, cx))
							continue; /* no shadow on sky, or on lava or a flame: it is the light */
						if (prev == 0)
							image[at] = shade(image[at], 52);
						shadowed[at] = (uint8_t)k;
					}
				}
			}
		}
	}
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
	*width = out_w;
	*out_height = out_h;
	return image;
}
