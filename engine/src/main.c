/* RemasterIA engine: runs a game project on the desktop (macOS first; Android later through SDL2).
 *
 * Today the SNES hardware comes from a libretro core (snes9x with snesre's tracer); the plan is to
 * replace its CPU, function by function, with the recompiled game code, while this layer grows the
 * enhancements (interpolated motion, layered rendering, widescreen).
 *
 *   engine                              inside an .app: Resources/{core.dylib, game.sfc, game.cfg}
 *   engine CORE ROM DATA_DIR [FRAMES]   development; with FRAMES, runs headless (records the intro)
 *
 * game.cfg has `key=value` lines: `title` names the window and the save folder, `video` picks the
 * look (pixel, crt; see video.c), `motion` smooth (in-between frames on 120 Hz displays, see motion.c)
 * or original. `remaster=on` redraws the picture in the engine's hard-pixel pass (remaster.c).
 * `art_styles`, when a game defines one, lists packs in Resources
 * (hdpack-<estilo>.bin, see hdpack.c). `art_capture` on saves frames with new tiles to
 * DATA_DIR/arte, for that game's own art work. `lights` names the game's list of lights by tile in
 * Resources (remaster.c); `light_detect=off` keeps to that list; `light_capture=on` saves the
 * tiles detection takes for a light to DATA_DIR/luzes, for review (tools/remasteria lights). Every
 * other key is passed to the core as an option (e.g. snes9x_mode7_hires=4x_hv). Shaders come from
 * Resources/shaders, or $REMASTERIA_SHADERS; in development $REMASTERIA_HDPACK (a folder of packs)
 * and $REMASTERIA_ART_CAPTURE (a folder) do the same for the art, $REMASTERIA_LIGHTS (a file) and
 * $REMASTERIA_LIGHT_CAPTURE (a folder) for the lights.
 *
 * Saves (cartridge battery, resume point, state slots), control bindings and the execution trace
 * live in DATA_DIR, by default ~/Library/Application Support/RemasterIA/<title>/.
 *
 * Controls: keyboard and gamepads (see input.c), remappable in the pause menu (Esc, the gamepad's
 * Home button or Select+Start), which also saves and loads the state slots (see menu.c).
 * Shortcuts: Tab (hold)=fast forward, F2=next look, F5=save to the current slot, F7=load it,
 * F11 or Cmd+F=fullscreen.
 */

#include <SDL.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <time.h>

#include "hdpack.h"
#include "input.h"
#include "remaster.h"
#include "menu.h"
#include "motion.h"
#include "video.h"

#define ENV_GET_CAN_DUPE 3
#define ENV_GET_SYSTEM_DIRECTORY 9
#define ENV_SET_PIXEL_FORMAT 10
#define ENV_GET_VARIABLE 15
#define ENV_GET_VARIABLE_UPDATE 17
#define ENV_GET_SAVE_DIRECTORY 31
#define ENV_EXPERIMENTAL 0x10000
#define DEVICE_JOYPAD 1
#define MEMORY_SAVE_RAM 0
#define MAX_OPTIONS 64
#define FAST_FORWARD_FRAMES 4
#define PATH 4096

struct game_info { const char *path; const void *data; size_t size; const char *meta; };
struct system_info { const char *name, *version, *extensions; bool need_fullpath, block_extract; };
struct av_info {
	unsigned base_width, base_height, max_width, max_height;
	float aspect;
	double fps, sample_rate;
};
struct variable { const char *key, *value; };

static struct {
	void (*init)(void);
	void (*deinit)(void);
	void (*set_environment)(bool (*)(unsigned, void *));
	void (*set_video_refresh)(void (*)(const void *, unsigned, unsigned, size_t));
	void (*set_audio_sample)(void (*)(int16_t, int16_t));
	void (*set_audio_sample_batch)(size_t (*)(const int16_t *, size_t));
	void (*set_input_poll)(void (*)(void));
	void (*set_input_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
	void (*get_system_info)(struct system_info *);
	void (*get_system_av_info)(struct av_info *);
	bool (*load_game)(const struct game_info *);
	void (*unload_game)(void);
	void (*run)(void);
	void (*reset)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	void (*trace_save)(void);
} core;

static char title[256] = "RemasterIA";
static char look[32] = "pixel";
static char *option_keys[MAX_OPTIONS], *option_values[MAX_OPTIONS];
static int options;
static char data_dir[PATH];

static SDL_Window *window;
static bool video_ready;
static float aspect = 4.0f / 3.0f;
static SDL_AudioDeviceID audio;
static int headless_button = BTN_START;
static char lights_file[PATH], light_dir[PATH];
static bool light_capture;
static bool fast_forward, quit, headless_start, smooth_motion = true, art_capture, remaster_on;
static char art_choice[64], art_styles[256];
static int pixel_format;
static uint32_t thumb[THUMB_WIDTH * THUMB_HEIGHT];
static const CoreMotion *(*core_motion)(void);
static double remaster_ms;
static long remaster_calls;
static bool last_frame_remastered;

/* Ahead: with two refreshes per game frame, a frame's remaster runs in the background while the
 * frame before it is on screen, and its in-between picture is drawn on the next refresh. Emulation,
 * remaster and in-between together are longer than one refresh; split this way each refresh only
 * waits for its own part. */
static struct {
	bool on, pending;
	uint32_t pixels[256 * 240];
	unsigned height;
	const CoreMotion *m;
	const uint32_t *hd;
	unsigned hd_width, hd_height;
	double ms;
#ifdef __APPLE__
	dispatch_semaphore_t done;
#endif
} ahead;

static void ahead_run(void *unused)
{
	(void)unused;
	Uint64 t0 = SDL_GetPerformanceCounter();
	ahead.hd = remaster_frame(ahead.pixels, 256 * sizeof(uint32_t), ahead.height, ahead.m, &ahead.hd_width, &ahead.hd_height);
	ahead.ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
#ifdef __APPLE__
	dispatch_semaphore_signal(ahead.done);
#endif
}

static void ahead_start(const void *data, size_t pitch, unsigned height, const CoreMotion *m)
{
	for (unsigned y = 0; y < height; y++)
		memcpy(&ahead.pixels[y * 256], (const char *)data + y * pitch, 256 * sizeof(uint32_t));
	ahead.height = height;
	ahead.m = m;
	ahead.pending = true;
	remaster_use_scale(video_fit_scale());
#ifdef __APPLE__
	if (!ahead.done)
		ahead.done = dispatch_semaphore_create(0);
	dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), NULL, ahead_run);
#else
	ahead_run(NULL);
#endif
}

/* Waits for the background remaster and keeps the frame for motion; returns the picture to show
 * when there is no in-between. The core has not run since, so its CoreMotion is still this frame's. */
static const uint32_t *ahead_finish(unsigned *width, unsigned *height)
{
#ifdef __APPLE__
	dispatch_semaphore_wait(ahead.done, DISPATCH_TIME_FOREVER);
#endif
	ahead.pending = false;
	remaster_use_scale(0);
	const uint32_t *hd = ahead.hd;
	unsigned w = ahead.hd_width, h = ahead.hd_height;
	last_frame_remastered = hd != NULL;
	if (hd) {
		remaster_ms += ahead.ms;
		remaster_calls++;
	} else {
		hd = hdpack_render(ahead.pixels, 256 * sizeof(uint32_t), ahead.height, ahead.m, &w, &h);
	}
	motion_capture(ahead.pixels, 256, ahead.height, 256 * sizeof(uint32_t), hd, hd ? w / 256 : 0);
	*width = hd ? w : 256;
	*height = hd ? h : ahead.height;
	return hd ? hd : ahead.pixels;
}

static bool environment(unsigned cmd, void *data)
{
	switch (cmd & ~ENV_EXPERIMENTAL) {
	case ENV_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case ENV_GET_SYSTEM_DIRECTORY:
	case ENV_GET_SAVE_DIRECTORY:
		*(const char **)data = data_dir;
		return true;
	case ENV_SET_PIXEL_FORMAT:
		pixel_format = *(const int *)data;
		video_set_format(pixel_format);
		return true;
	case ENV_GET_VARIABLE: {
		struct variable *var = data;
		for (int i = 0; i < options; i++)
			if (var->key && !strcmp(var->key, option_keys[i])) {
				var->value = option_values[i];
				return true;
			}
		return false;
	}
	case ENV_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	default:
		return false;
	}
}

static uint32_t to_rgba(const void *row, unsigned x)
{
	unsigned r, g, b;
	if (pixel_format == VIDEO_XRGB8888) {
		uint32_t p = ((const uint32_t *)row)[x];
		r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
	} else if (pixel_format == VIDEO_RGB565) {
		uint16_t p = ((const uint16_t *)row)[x];
		r = (p >> 11 & 0x1F) * 255 / 31, g = (p >> 5 & 0x3F) * 255 / 63, b = (p & 0x1F) * 255 / 31;
	} else {
		uint16_t p = ((const uint16_t *)row)[x];
		r = (p >> 10 & 0x1F) * 255 / 31, g = (p >> 5 & 0x1F) * 255 / 31, b = (p & 0x1F) * 255 / 31;
	}
	return menu_rgba(r, g, b, 255);
}

/* Keeps a small copy of the latest frame, saved next to each state as its picture in the menu. */
static void update_thumb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	for (unsigned y = 0; y < THUMB_HEIGHT; y++) {
		const char *row = (const char *)data + (size_t)((y * 2 + 1) * height / (THUMB_HEIGHT * 2)) * pitch;
		for (unsigned x = 0; x < THUMB_WIDTH; x++)
			thumb[y * THUMB_WIDTH + x] = to_rgba(row, (x * 2 + 1) * width / (THUMB_WIDTH * 2));
	}
}

static uint32_t console_copy[512 * 480];
static unsigned console_copy_w, console_copy_h;

/* Keeps the latest console picture so the capture frame can be saved beside the remaster. */
static void stash_console(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (pixel_format != VIDEO_XRGB8888 || (!getenv("REMASTERIA_CAPTURE") && !light_dir[0]) || !data
		|| width == 0 || width > 512 || height == 0 || height > 480)
		return;
	if (pitch < width * sizeof(uint32_t))
		return;
	console_copy_w = width;
	console_copy_h = height;
	for (unsigned y = 0; y < height; y++) {
		const uint32_t *row = (const uint32_t *)((const char *)data + (size_t)y * pitch);
		memcpy(console_copy + (size_t)y * width, row, (size_t)width * sizeof(uint32_t));
	}
}

static void write_console_to(const char *path)
{
	if (!path || !console_copy_w || !console_copy_h)
		return;
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", console_copy_w, console_copy_h);
	for (unsigned y = 0; y < console_copy_h; y++) {
		const uint32_t *row = console_copy + (size_t)y * console_copy_w;
		for (unsigned x = 0; x < console_copy_w; x++) {
			uint32_t p = row[x];
			unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void write_console(void)
{
	write_console_to(getenv("REMASTERIA_CAPTURE_SRC"));
}

/* Light capture: each tile detection takes for a light, the first time it is seen, with the
 * console picture it was in. candidates.tsv: key kind luma r g b radius picture x0 y0 x1 y1. */
static uint64_t *light_seen;
static size_t light_seen_mask, light_seen_used;
static long light_session;

static bool light_seen_add(uint64_t key)
{
	if ((light_seen_used + 1) * 2 > light_seen_mask + 1 || !light_seen) {
		size_t size = light_seen ? (light_seen_mask + 1) * 2 : 1024;
		uint64_t *bigger = calloc(size, sizeof *bigger);
		for (size_t i = 0; light_seen && i <= light_seen_mask; i++)
			if (light_seen[i]) {
				size_t j = light_seen[i] & (size - 1);
				while (bigger[j])
					j = (j + 1) & (size - 1);
				bigger[j] = light_seen[i];
			}
		free(light_seen);
		light_seen = bigger;
		light_seen_mask = size - 1;
	}
	size_t i = key & light_seen_mask;
	while (light_seen[i] && light_seen[i] != key)
		i = (i + 1) & light_seen_mask;
	if (light_seen[i])
		return false;
	light_seen[i] = key;
	light_seen_used++;
	return true;
}

static void light_capture_init(void)
{
	mkdir(light_dir, 0755);
	char path[PATH + 32];
	snprintf(path, sizeof path, "%s/candidates.tsv", light_dir);
	FILE *f = fopen(path, "r");
	char line[256];
	unsigned long long key;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "%llx", &key) == 1 && key)
			light_seen_add(key);
	if (f)
		fclose(f);
	light_session = (long)time(NULL);
	remaster_find_lights(true);
}

static void light_capture_frame(const CoreMotion *m)
{
	int n;
	const LightFound *found = remaster_lights_found(&n);
	char picture[64] = "", path[PATH + 64];
	FILE *list = NULL;
	for (int i = 0; i < n; i++) {
		if (!light_seen_add(found[i].key))
			continue;
		if (!picture[0]) {
			snprintf(picture, sizeof picture, "%ld-%07u.ppm", light_session, m->frame);
			snprintf(path, sizeof path, "%s/%s", light_dir, picture);
			write_console_to(path);
			snprintf(path, sizeof path, "%s/candidates.tsv", light_dir);
			list = fopen(path, "a");
		}
		if (list)
			fprintf(list, "%016llx\t%c\t%u\t%u\t%u\t%u\t%d\t%s\t%d\t%d\t%d\t%d\n", (unsigned long long)found[i].key,
				found[i].kind, found[i].luma, found[i].r, found[i].g, found[i].b, found[i].radius, picture,
				found[i].x0, found[i].y0, found[i].x1, found[i].y1);
	}
	if (list)
		fclose(list);
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data)
		return;
	const CoreMotion *m = pixel_format == VIDEO_XRGB8888 && core_motion ? core_motion() : NULL;
	bool tiles = m && m->valid && width == 256 && height + m->top <= 240;
	if (tiles && art_capture && !fast_forward)
		hdpack_capture(data, pitch, height, m);
	if (!video_ready) {
		stash_console(data, width, height, pitch);
		if (tiles && light_dir[0]) {
			unsigned w, h;
			if (remaster_frame(data, pitch, height, m, &w, &h))
				light_capture_frame(m);
		}
		return;
	}
	unsigned hd_width, hd_height;
	/* The game project turns the remaster on. REMASTERIA_RAW keeps the console frame. */
	bool draw_remaster = (remaster_on || getenv("REMASTERIA_REMASTER")) && !getenv("REMASTERIA_RAW");
	if (ahead.on && tiles && draw_remaster && !art_capture && !light_dir[0]) {
		ahead_start(data, pitch, height, m);
		update_thumb(data, width, height, pitch);
		stash_console(data, width, height, pitch);
		return;
	}
	Uint64 remaster_t0 = SDL_GetPerformanceCounter();
	const uint32_t *hd = NULL;
	last_frame_remastered = false;
	if (tiles && draw_remaster)
		hd = remaster_frame(data, pitch, height, m, &hd_width, &hd_height);
	last_frame_remastered = hd != NULL;
	if (hd) {
		remaster_ms += (SDL_GetPerformanceCounter() - remaster_t0) * 1000.0 / SDL_GetPerformanceFrequency();
		remaster_calls++;
	}
	if (!hd && tiles)
		hd = hdpack_render(data, pitch, height, m, &hd_width, &hd_height);
	if (hd)
		video_frame(hd, hd_width, hd_height, hd_width * sizeof(uint32_t));
	else
		video_frame(data, width, height, pitch);
	update_thumb(data, width, height, pitch);
	if (pixel_format == VIDEO_XRGB8888)
		motion_capture(data, width, height, pitch, hd, hd ? hd_width / 256 : 0);
	stash_console(data, width, height, pitch);
	if (hd && last_frame_remastered && light_dir[0] && !fast_forward)
		light_capture_frame(m);
}

/* Dynamic rate control: the game runs locked to the display's refresh, which is never exactly the
 * console's 60.1 Hz, so the audio is resampled by up to ±0.5% (inaudible) to keep the output queue
 * at its target instead of running dry (crackles) or growing (lag). */
#define MAX_RATE_DEVIATION 0.005
static double audio_ratio = 1.0, audio_position;
static int16_t audio_last[2];
static Uint32 audio_target_bytes;
static bool audio_waiting = true;

static void update_audio_ratio(void)
{
	if (!audio)
		return;
	Uint32 queued = SDL_GetQueuedAudioSize(audio);
	if (queued == 0 && !audio_waiting) {  /* ran dry: refill before playing again */
		audio_waiting = true;
		SDL_PauseAudioDevice(audio, 1);
	} else if (audio_waiting && queued >= audio_target_bytes) {
		audio_waiting = false;
		SDL_PauseAudioDevice(audio, 0);
	} else if (queued > 4 * audio_target_bytes) {
		SDL_ClearQueuedAudio(audio);
	}
	double error = ((double)audio_target_bytes - queued) / audio_target_bytes;
	audio_ratio = 1.0 + MAX_RATE_DEVIATION * (error > 1 ? 1 : error < -1 ? -1 : error);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
	if (!audio || fast_forward || !frames)
		return frames;
	static int16_t out[8192 * 2];
	size_t produced = 0;
	double step = 1.0 / audio_ratio;
	/* Linear interpolation between consecutive stereo frames; position -1 is the last frame of the
	 * previous batch, so batches join without clicks. */
	while (audio_position < frames - 1 && produced < sizeof out / sizeof out[0] / 2) {
		long i = (long)(audio_position >= 0 ? audio_position : -1);
		double t = audio_position - i;
		const int16_t *a = i < 0 ? audio_last : data + 2 * i, *b = data + 2 * (i + 1);
		out[2 * produced] = (int16_t)(a[0] + (b[0] - a[0]) * t);
		out[2 * produced + 1] = (int16_t)(a[1] + (b[1] - a[1]) * t);
		produced++;
		audio_position += step;
	}
	audio_position -= frames;
	audio_last[0] = data[2 * (frames - 1)], audio_last[1] = data[2 * (frames - 1) + 1];
	SDL_QueueAudio(audio, out, (Uint32)(produced * 4));
	return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
	int16_t sample[2] = { left, right };
	audio_batch(sample, 1);
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (device != DEVICE_JOYPAD || id >= BUTTONS)
		return 0;
	if (headless_start)
		return port == 0 && (int)id == headless_button;
	return input_down((int)port, (int)id);
}

static void *load_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *data = malloc(*size ? *size : 1);
	if (fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static void write_file(const char *path, const void *data, size_t size)
{
	char tmp[PATH + 8];
	snprintf(tmp, sizeof tmp, "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	fwrite(data, 1, size, f);
	fclose(f);
	rename(tmp, path);
}

static void save_battery(const char *path)
{
	void *data = core.get_memory_data(MEMORY_SAVE_RAM);
	size_t size = core.get_memory_size(MEMORY_SAVE_RAM);
	if (data && size)
		write_file(path, data, size);
}

static bool save_state(const char *path)
{
	size_t size = core.serialize_size();
	void *data = malloc(size);
	bool ok = data && core.serialize(data, size);
	if (ok)
		write_file(path, data, size);
	free(data);
	return ok;
}

static bool load_state(const char *path)
{
	size_t size;
	void *data = load_file(path, &size);
	bool ok = data && core.unserialize(data, size);
	free(data);
	return ok;
}

static void read_config(const char *path)
{
	size_t size;
	char *text = load_file(path, &size);
	if (!text)
		return;
	text = realloc(text, size + 1);
	text[size] = 0;
	for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		if (!strcmp(line, "title"))
			snprintf(title, sizeof title, "%s", eq + 1);
		else if (!strcmp(line, "video"))
			snprintf(look, sizeof look, "%s", eq + 1);
		else if (!strcmp(line, "motion"))
			smooth_motion = strcmp(eq + 1, "original") != 0;
		else if (!strcmp(line, "art"))
			snprintf(art_choice, sizeof art_choice, "%s", eq + 1);
		else if (!strcmp(line, "art_styles"))
			snprintf(art_styles, sizeof art_styles, "%s", eq + 1);
		else if (!strcmp(line, "remaster"))
			remaster_on = !strcmp(eq + 1, "on");
		else if (!strcmp(line, "art_capture"))
			art_capture = !strcmp(eq + 1, "on");
		else if (!strcmp(line, "lights"))
			snprintf(lights_file, sizeof lights_file, "%s", eq + 1);
		else if (!strcmp(line, "light_detect"))
			remaster_detect_lights(strcmp(eq + 1, "off") != 0);
		else if (!strcmp(line, "light_capture"))
			light_capture = !strcmp(eq + 1, "on");
		else if (options < MAX_OPTIONS)
			option_keys[options] = strdup(line), option_values[options++] = strdup(eq + 1);
	}
	free(text);
}

static void slot_path(int slot, const char *kind, char *out, size_t size)
{
	snprintf(out, size, "%s/slot-%d.%s", data_dir, slot, kind);
}

static bool save_slot(int slot)
{
	char path[PATH + 32];
	slot_path(slot, "state", path, sizeof path);
	if (!save_state(path))
		return false;
	slot_path(slot, "thumb", path, sizeof path);
	write_file(path, thumb, sizeof thumb);
	return true;
}

static bool load_slot(int slot)
{
	char path[PATH + 32];
	slot_path(slot, "state", path, sizeof path);
	return load_state(path);
}

static bool slot_info(int slot, char *when, size_t size, uint32_t *picture)
{
	char path[PATH + 32];
	struct stat st;
	slot_path(slot, "state", path, sizeof path);
	if (stat(path, &st) != 0)
		return false;
	strftime(when, size, "%d/%m/%Y %H:%M", localtime(&st.st_mtime));
	slot_path(slot, "thumb", path, sizeof path);
	size_t got;
	void *data = load_file(path, &got);
	if (data && got == sizeof thumb)
		memcpy(picture, data, got);
	else
		memset(picture, 0, sizeof thumb);
	free(data);
	return true;
}

static void update_title(void);

static void toggle_fullscreen(void)
{
	bool full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
	if (!full) {
		int x, y, w, h;
		SDL_GetWindowPosition(window, &x, &y);
		SDL_GetWindowSize(window, &w, &h);
		video_native_display(x + w / 2, y + h / 2);
	}
	SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
}

static void next_look(void)
{
	video_next_profile();
	update_title();
}

static void next_motion(void)
{
	smooth_motion = !smooth_motion;
}

static const char *motion_name(void)
{
	return smooth_motion ? "suave" : "original";
}

/* Loads hdpack-<id>.bin from a directory, in the order the game asked for. A plain file loads as
 * the one style it contains. */
static void load_art_styles(const char *dir, const char *list)
{
	struct stat st;
	if (stat(dir, &st) == 0 && S_ISREG(st.st_mode)) {
		hdpack_load(dir);
		return;
	}
	char clean[PATH], path[PATH];
	snprintf(clean, sizeof clean, "%s", dir);
	size_t len = strlen(clean);
	if (len && clean[len - 1] == '/')
		clean[len - 1] = 0;
	if (!list || !list[0])
		return;
	char owned[256];
	snprintf(owned, sizeof owned, "%s", list);
	for (char *id = strtok(owned, ","); id; id = strtok(NULL, ",")) {
		while (*id == ' ')
			id++;
		snprintf(path, sizeof path, "%s/hdpack-%s.bin", clean, id);
		hdpack_load(path);
	}
}

static void request_quit(void)
{
	quit = true;
}

/* The console's reset button: the game starts over, the cartridge saves stay. */
static void reset_game(void)
{
	core.reset();
	motion_reset();
}

/* Automated visual checks: REMASTERIA_CAPTURE=out.ppm REMASTERIA_CAPTURE_FRAME=N saves frame N and quits;
 * REMASTERIA_CAPTURE_MENU=main|save|load|controls shows that menu screen in the capture. */
static bool capture_mode(void)
{
	return getenv("REMASTERIA_CAPTURE") && getenv("REMASTERIA_CAPTURE_FRAME");
}

static void update_title(void)
{
	char text[512];
	snprintf(text, sizeof text, "%s — %s", title, video_profile_name());
	SDL_SetWindowTitle(window, text);
}

/* True once the visible window has actually entered desktop fullscreen.
 * Position is not compared: on this display the fullscreen space reports a
 * different origin than SDL_GetDisplayBounds, and treating that as failure
 * cancelled fullscreen on every launch. */
static bool window_covers_display(void)
{
	return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
	exit(1);
}

/* The first frames are the slow ones: the driver finishes the shaders on the first draw,
 * and the remaster's picture is uploaded for the first time. They are drawn off the window,
 * so the screen remains black, and the game is then rewound to the real start. */
static void warm_up(double refresh_hz)
{
	size_t bytes = core.serialize_size();
	void *shot = bytes ? malloc(bytes) : NULL;
	if (!shot || !core.serialize(shot, bytes)) {
		free(shot);
		return;
	}
	bool want_remaster = remaster_on || getenv("REMASTERIA_REMASTER") != NULL;
	double budget = 2.0 * 1000.0 / (refresh_hz > 1 ? refresh_hz : 60);
	int fast = 0;
	for (int i = 0; i < 40; i++) {
		SDL_PumpEvents();
		Uint64 t0 = SDL_GetPerformanceCounter();
		core.run();
		video_preload();
		double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
		bool counts = !want_remaster || last_frame_remastered;
		fast = counts && ms < budget ? fast + 1 : 0;
		if (i >= 2 && fast >= 2)
			break;
	}
	core.unserialize(shot, bytes);
	free(shot);
	motion_reset();
	audio_position = 0;
	audio_ratio = 1;
	audio_last[0] = audio_last[1] = 0;
	if (audio) {
		SDL_ClearQueuedAudio(audio);
		SDL_PauseAudioDevice(audio, 1);
	}
	audio_waiting = true;
}

#define LOAD(field, name) \
	if (!(*(void **)&core.field = dlsym(lib, name))) fail("núcleo incompleto: falta " name);

int main(int argc, char **argv)
{
	char core_path[PATH], rom_path[PATH], shader_dir[PATH] = "", controller_db[PATH] = "", resource_dir[PATH] = "";
	long headless_frames = 0;

	if (argc == 1) {
		char *base = SDL_GetBasePath();
		if (!base)
			fail("não achei a pasta de recursos do aplicativo");
		char config[PATH];
		snprintf(config, sizeof config, "%sgame.cfg", base);
		snprintf(core_path, sizeof core_path, "%score.dylib", base);
		snprintf(rom_path, sizeof rom_path, "%sgame.sfc", base);
		snprintf(shader_dir, sizeof shader_dir, "%sshaders", base);
		snprintf(controller_db, sizeof controller_db, "%sgamecontrollerdb.txt", base);
		snprintf(resource_dir, sizeof resource_dir, "%s", base);
		read_config(config);
		SDL_free(base);
		char *pref = SDL_GetPrefPath("RemasterIA", title);
		if (!pref)
			fail("não consegui criar a pasta de saves");
		snprintf(data_dir, sizeof data_dir, "%s", pref);
		SDL_free(pref);
		size_t len = strlen(data_dir);
		if (len && data_dir[len - 1] == '/')
			data_dir[len - 1] = 0;
		if (!getenv("SNESRE_TRACE")) {
			char trace[PATH + 16];
			snprintf(trace, sizeof trace, "%s/trace.bin", data_dir);
			setenv("SNESRE_TRACE", trace, 1);
		}
	} else if (argc == 4 || argc == 5) {
		snprintf(core_path, sizeof core_path, "%s", argv[1]);
		snprintf(rom_path, sizeof rom_path, "%s", argv[2]);
		snprintf(data_dir, sizeof data_dir, "%s", argv[3]);
		headless_frames = argc == 5 ? atol(argv[4]) : 0;
	} else {
		fprintf(stderr, "uso: %s [NÚCLEO ROM PASTA_DE_DADOS [QUADROS]]\n", argv[0]);
		return 2;
	}
	if (argc != 1 && getenv("REMASTERIA_CONFIG"))
		read_config(getenv("REMASTERIA_CONFIG"));
	if (getenv("REMASTERIA_VIDEO"))
		snprintf(look, sizeof look, "%s", getenv("REMASTERIA_VIDEO"));
	if (getenv("REMASTERIA_SHADERS"))
		snprintf(shader_dir, sizeof shader_dir, "%s", getenv("REMASTERIA_SHADERS"));
	if (getenv("REMASTERIA_CONTROLLER_DB"))
		snprintf(controller_db, sizeof controller_db, "%s", getenv("REMASTERIA_CONTROLLER_DB"));
	if (getenv("REMASTERIA_ART"))
		snprintf(art_choice, sizeof art_choice, "%s", getenv("REMASTERIA_ART"));
	if (getenv("REMASTERIA_HDPACK"))
		load_art_styles(getenv("REMASTERIA_HDPACK"), art_styles[0] ? art_styles : "remaster");
	else if (art_styles[0] && resource_dir[0])
		load_art_styles(resource_dir, art_styles);
	if (art_choice[0])
		hdpack_use(art_choice);
	char capture_dir[PATH + 16] = "";
	if (getenv("REMASTERIA_ART_CAPTURE"))
		snprintf(capture_dir, sizeof capture_dir, "%s", getenv("REMASTERIA_ART_CAPTURE"));
	else if (art_capture)
		snprintf(capture_dir, sizeof capture_dir, "%s/arte", data_dir);
	if (capture_dir[0]) {
		mkdir(capture_dir, 0755);
		hdpack_capture_init(capture_dir);
		art_capture = true;
	}
	char lights_path[PATH * 2 + 2] = "";
	if (getenv("REMASTERIA_LIGHTS"))
		snprintf(lights_path, sizeof lights_path, "%s", getenv("REMASTERIA_LIGHTS"));
	else if (lights_file[0] && resource_dir[0])
		snprintf(lights_path, sizeof lights_path, "%s%s", resource_dir, lights_file);
	if (lights_path[0])
		remaster_load_lights(lights_path);
	if (getenv("REMASTERIA_LIGHT_DETECT"))
		remaster_detect_lights(strcmp(getenv("REMASTERIA_LIGHT_DETECT"), "off") != 0);
	if (getenv("REMASTERIA_LIGHT_CAPTURE"))
		snprintf(light_dir, sizeof light_dir, "%s", getenv("REMASTERIA_LIGHT_CAPTURE"));
	else if (light_capture)
		snprintf(light_dir, sizeof light_dir, "%s/luzes", data_dir);
	if (light_dir[0])
		light_capture_init();

	void *lib = dlopen(core_path, RTLD_NOW);
	if (!lib)
		fail(dlerror());
	LOAD(init, "retro_init") LOAD(deinit, "retro_deinit") LOAD(set_environment, "retro_set_environment")
	LOAD(set_video_refresh, "retro_set_video_refresh") LOAD(set_audio_sample, "retro_set_audio_sample")
	LOAD(set_audio_sample_batch, "retro_set_audio_sample_batch") LOAD(set_input_poll, "retro_set_input_poll")
	LOAD(set_input_state, "retro_set_input_state") LOAD(get_system_info, "retro_get_system_info")
	LOAD(get_system_av_info, "retro_get_system_av_info") LOAD(load_game, "retro_load_game")
	LOAD(unload_game, "retro_unload_game") LOAD(run, "retro_run") LOAD(reset, "retro_reset") LOAD(serialize_size, "retro_serialize_size")
	LOAD(serialize, "retro_serialize") LOAD(unserialize, "retro_unserialize")
	LOAD(get_memory_data, "retro_get_memory_data") LOAD(get_memory_size, "retro_get_memory_size")
	*(void **)&core.trace_save = dlsym(lib, "snesre_trace_save");
	*(void **)&core_motion = dlsym(lib, "retro_remasteria_motion");
	motion_init(core_motion);

	char battery_path[PATH + 16], resume_path[PATH + 16];
	snprintf(battery_path, sizeof battery_path, "%s/game.srm", data_dir);
	snprintf(resume_path, sizeof resume_path, "%s/resume.state", data_dir);

	if (SDL_Init(headless_frames ? 0 : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
		fail(SDL_GetError());
	core.set_environment(environment);
	core.init();
	core.set_video_refresh(video);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);

	struct system_info system = { 0 };
	core.get_system_info(&system);
	size_t rom_size = 0;
	void *rom = system.need_fullpath ? NULL : load_file(rom_path, &rom_size);
	struct game_info game = { rom_path, rom, rom_size, NULL };
	if ((!rom && !system.need_fullpath) || !core.load_game(&game))
		fail("não consegui abrir o jogo");

	size_t battery_size;
	void *battery = load_file(battery_path, &battery_size);
	if (battery && core.get_memory_data(MEMORY_SAVE_RAM) && battery_size == core.get_memory_size(MEMORY_SAVE_RAM))
		memcpy(core.get_memory_data(MEMORY_SAVE_RAM), battery, battery_size);
	free(battery);

	struct av_info av = { 0 };
	core.get_system_av_info(&av);
	if (av.base_width && av.base_height)
		aspect = (float)av.base_width / av.base_height;  /* square pixels */
	double fps = av.fps > 0 ? av.fps : 60.0;

	if (headless_frames) {
		bool idle = getenv("REMASTERIA_HEADLESS_IDLE") != NULL;  /* let the intro play instead of pressing Start */
		/* A survey of the game for testing the remaster: every N frames, the console
		 * picture and a state to resume from. */
		const char *survey = getenv("REMASTERIA_SURVEY");
		long every = getenv("REMASTERIA_SURVEY_EVERY") ? atol(getenv("REMASTERIA_SURVEY_EVERY")) : 300;
		bool press_a = getenv("REMASTERIA_SURVEY_PRESS_A") != NULL;  /* A after the title, to get past menus and dialog */
		for (long i = 0; i < headless_frames; i++) {
			headless_start = !idle && i % 120 < 4;
			headless_button = press_a && i >= 600 ? BTN_A : BTN_START;
			core.run();
			if (survey && every > 0 && i % every == every - 1) {
				char path[PATH + 32];
				snprintf(path, sizeof path, "%s/f%06ld.ppm", survey, i + 1);
				write_console_to(path);
				snprintf(path, sizeof path, "%s/f%06ld.state", survey, i + 1);
				save_state(path);
			}
		}
		save_battery(battery_path);
		hdpack_capture_save();
		core.unload_game();
		core.deinit();
		SDL_Quit();
		return 0;
	}

	if (argc == 1)
		load_state(resume_path);
	else if (capture_mode() && getenv("REMASTERIA_CAPTURE_STATE"))
		load_state(getenv("REMASTERIA_CAPTURE_STATE"));

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	int window_w = (int)(224 * 3 * aspect), window_h = 224 * 3;
	Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL;
	if (!capture_mode()) {
		/* Open already covering the display. Growing a smaller window into
		 * fullscreen afterwards is the resolution change and the hitch at launch. */
		window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0) {
			video_native_display(bounds.x + bounds.w / 2, bounds.y + bounds.h / 2);
			SDL_PumpEvents();
			if (SDL_GetDisplayBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0)
				window_w = bounds.w, window_h = bounds.h;
		}
	} else {
		window_w = 256 * 6, window_h = 240 * 6;
	}
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		window_w, window_h, window_flags);
	if (!window)
		fail(SDL_GetError());
	SDL_ShowCursor(SDL_DISABLE);
	video_ready = video_init(window, shader_dir[0] ? shader_dir : NULL, look);
	if (!video_ready)
		fail("não consegui iniciar o vídeo (OpenGL e shaders)");
	video_set_vsync(false);
	if (!capture_mode())
		video_blank(); /* black from here on; the game is not drawn until it is warmed up */
	update_title();
	input_init(data_dir, controller_db[0] ? controller_db : NULL);
	MenuActions actions = { save_slot, load_slot, slot_info, toggle_fullscreen, next_look, video_profile_name,
		next_motion, motion_name, reset_game, request_quit };
	menu_init(&actions, title);

	SDL_AudioSpec want = { 0 }, have;
	want.freq = av.sample_rate > 0 ? (int)av.sample_rate : 32040;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	audio_target_bytes = (Uint32)(want.freq * 4 / 20);  /* 50 ms queued */
	if (audio)
		SDL_PauseAudioDevice(audio, 1);

	if (!capture_mode()) {
		/* The window was created fullscreen. Asking again restarts the resize.
		 * Only a window that did not take the flag still needs the request. */
		if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0)
			SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
		Uint32 fullscreen_at = SDL_GetTicks();
		while (!window_covers_display() && SDL_GetTicks() - fullscreen_at < 1000) {
			SDL_PumpEvents();
			video_blank();
			SDL_Delay(16);
		}
		if (!window_covers_display()) {
			int index = SDL_GetWindowDisplayIndex(window);
			SDL_Rect bounds;
			if (index >= 0 && SDL_GetDisplayBounds(index, &bounds) == 0 && bounds.w > 0) {
				SDL_SetWindowFullscreen(window, 0);
				SDL_SetWindowBordered(window, SDL_FALSE);
				SDL_SetWindowPosition(window, bounds.x, bounds.y);
				SDL_SetWindowSize(window, bounds.w, bounds.h);
				SDL_PumpEvents();
				video_blank();
			}
		}
		SDL_DisplayMode desktop;
		int display = SDL_GetWindowDisplayIndex(window);
		int desktop_hz = display >= 0 && SDL_GetCurrentDisplayMode(display, &desktop) == 0 && desktop.refresh_rate > 0
			? desktop.refresh_rate : 60;
		SDL_PumpEvents();
		warm_up(desktop_hz);
		video_blank(); /* warmup never touched the window; this keeps the front buffer black */
		/* One real frame, drawn while the screen is still the black front buffer. */
		core.run();
		video_finish();
		SDL_PumpEvents();
		SDL_Event pending;
		while (SDL_PollEvent(&pending)) {
			if (pending.type == SDL_QUIT)
				quit = true;
		}
	}

	/* Frame pacing: lock the game to the display (vsync) when its refresh is a multiple of the
	 * game's rate (60 Hz, 120 Hz ProMotion...), otherwise fall back to a timer. Read this after
	 * the window is on screen; a hidden window does not report the display's real refresh. */
	SDL_DisplayMode mode;
	int display = SDL_GetWindowDisplayIndex(window);
	int refresh = SDL_GetWindowDisplayMode(window, &mode) == 0 && mode.refresh_rate > 0 ? mode.refresh_rate
		: display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 0 ? mode.refresh_rate
		: 60;
	int refreshes_per_frame = (int)(refresh / fps + 0.5);
	if (refreshes_per_frame < 1)
		refreshes_per_frame = 1;
	double locked_rate = (double)refresh / refreshes_per_frame;
	bool vsync_on = video_set_vsync(true);
	bool synced = !capture_mode() && locked_rate / fps > 0.98 && locked_rate / fps < 1.02 && vsync_on;
	/* Locked to the display, the game runs at its refresh rate (60.0 instead of 60.1 Hz): every
	 * frame then lands on a refresh, with no periodic double step; audio rate control absorbs it. */
	double rate = synced ? locked_rate : fps;
	/* With two refreshes per game frame, the second shows the next frame and the first one the
	 * halfway point between them (motion.c). */
	bool can_interpolate = motion_available() && ((synced && refreshes_per_frame == 2) || getenv("REMASTERIA_CAPTURE_BETWEEN"));
	bool show_current = false;
	long between_frames = 0;

	Uint64 frequency = SDL_GetPerformanceFrequency();
	Uint64 clock_start;
	if (!capture_mode()) {
		/* The opening frame is already drawn. This swap is the first picture on screen. */
		video_present();
		clock_start = SDL_GetPerformanceCounter();
	} else {
		clock_start = SDL_GetPerformanceCounter() - frequency / (Uint64)(2 * rate);  /* half-frame phase */
	}
	long frames_done = 0;
	double core_ms = 0, between_ms = 0;
	Uint32 last_save = SDL_GetTicks();
	bool running = true, paused = false;
	long capture_frame = capture_mode() ? atol(getenv("REMASTERIA_CAPTURE_FRAME")) : 0;
	long frames_run = 0;
	bool show_stats = getenv("REMASTERIA_STATS") != NULL;  /* pacing diagnostics on stdout */
	Uint32 stats_start = SDL_GetTicks();
	long frames_run_stats = 0;
	bool capture_between = false;
	while (running) {
		if (ahead.pending) {
			/* The refresh after a frame's emulation: its in-between picture. */
			unsigned pw, ph, mw, mh;
			const uint32_t *picture = ahead_finish(&pw, &ph), *middle;
			Uint64 t0 = SDL_GetPerformanceCounter();
			if ((middle = motion_between(&mw, &mh))) {
				between_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / frequency;
				video_frame(middle, mw, mh, mw * sizeof(uint32_t));
				between_frames++;
			} else {
				video_frame(picture, pw, ph, pw * sizeof(uint32_t));
			}
			if (capture_between) {
				if (getenv("REMASTERIA_CAPTURE_MENU"))
					menu_show(getenv("REMASTERIA_CAPTURE_MENU"));
				video_capture_next(getenv("REMASTERIA_CAPTURE"));
			}
			video_set_overlay(menu_draw(), MENU_WIDTH, MENU_HEIGHT);
			video_present();
			if (capture_between) {
				write_console();
				if (getenv("REMASTERIA_CAPTURE_SAVE"))
					save_state(getenv("REMASTERIA_CAPTURE_SAVE"));
				running = false;
			}
			continue;
		}
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (input_handle_event(&event, menu_toast) || menu_handle_event(&event))
				continue;
			if (event.type == SDL_QUIT)
				running = false;
			else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
				/* A focus change must not freeze the game. Only drop a backlog so it does not fast-forward. */
				clock_start = SDL_GetPerformanceCounter();
				frames_done = 0;
			}
			else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
				SDL_Keycode sym = event.key.keysym.sym;
				if (sym == SDLK_F2) {
					video_next_profile();
					update_title();
				} else if (sym == SDLK_F5 || sym == SDLK_F7) {
					char message[64];
					int slot = menu_slot();
					bool saving = sym == SDLK_F5, ok = saving ? save_slot(slot) : load_slot(slot);
					snprintf(message, sizeof message, ok ? "Slot %d %s" : "Slot %d: falhou ao %s", slot,
						saving ? (ok ? "salvo" : "salvar") : (ok ? "carregado" : "carregar"));
					menu_toast(message);
				} else if (sym == SDLK_F11 || (sym == SDLK_f && (event.key.keysym.mod & KMOD_GUI))) {
					toggle_fullscreen();
				}
			}
		}
		if (quit)
			running = false;
		if (paused) {
			SDL_Delay(30);
			continue;
		}
		input_update();
		fast_forward = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];
		if (!menu_is_open() && input_menu_combo())
			menu_open();
		if (menu_is_open()) {
			if (!audio_waiting) {
				audio_waiting = true;
				SDL_PauseAudioDevice(audio, 1);
			}
			menu_update();
			video_set_overlay(menu_draw(), MENU_WIDTH, MENU_HEIGHT);
			video_present();
			if (!synced)
				SDL_Delay(16);
			clock_start = SDL_GetPerformanceCounter() - frequency / (Uint64)(2 * rate);
			frames_done = 0;
			continue;
		}
		long due = capture_frame ? 1
			: (long)((SDL_GetPerformanceCounter() - clock_start) * rate / frequency) - frames_done;
		if (due > 3) {  /* fell far behind (window dragged, system busy): resync instead of rushing */
			clock_start = SDL_GetPerformanceCounter() - frequency / (Uint64)(2 * rate);
			frames_done = 0;
			due = 1;
		}
		bool run_frame = due > 0;
		ahead.on = can_interpolate && smooth_motion && !fast_forward && due == 1;
		for (long f = 0; f < due; f++) {
			Uint64 t0 = SDL_GetPerformanceCounter();
			if (capture_frame && getenv("REMASTERIA_CAPTURE_PRESS")) {
				/* the survey's presses, so a capture can reach a scene past the menus;
				 * "walk" holds the arrows in turn instead */
				static const int walk[] = { BTN_RIGHT, BTN_DOWN, BTN_LEFT, BTN_UP, BTN_LEFT, BTN_DOWN, BTN_RIGHT, BTN_UP };
				bool walking = strcmp(getenv("REMASTERIA_CAPTURE_PRESS"), "walk") == 0;
				headless_start = walking || frames_run % 120 < 4;
				headless_button = walking ? walk[frames_run / 100 % 8] : frames_run >= 600 ? BTN_A : BTN_START;
			}
			for (int i = 0; i < (fast_forward ? FAST_FORWARD_FRAMES : 1); i++)
				core.run();
			core_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / frequency;
			frames_done++;
			frames_run_stats++;
			update_audio_ratio();
			if (capture_frame && ++frames_run == capture_frame && ahead.pending) {
				capture_between = true;  /* the capture is this frame's in-between, drawn next */
			} else if (capture_frame && frames_run == capture_frame) {
				if (getenv("REMASTERIA_CAPTURE_MENU"))
					menu_show(getenv("REMASTERIA_CAPTURE_MENU"));
				video_capture_next(getenv("REMASTERIA_CAPTURE"));
				write_console();
				if (getenv("REMASTERIA_CAPTURE_SAVE"))
					save_state(getenv("REMASTERIA_CAPTURE_SAVE"));
				running = false;
			}
		}
		ahead.on = false;
		if (run_frame && ahead.pending) {
			/* This refresh still shows the frame before; the one just emulated is being drawn. */
			const uint32_t *now;
			unsigned nw, nh;
			if ((now = motion_current(&nw, &nh)))
				video_frame(now, nw, nh, nw * sizeof(uint32_t));
			show_current = false;
		} else if (run_frame) {
			const uint32_t *middle;
			unsigned mw, mh;
			Uint64 t0 = SDL_GetPerformanceCounter();
			show_current = false;
			if (can_interpolate && smooth_motion && !fast_forward && due == 1 && (middle = motion_between(&mw, &mh))) {
				between_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / frequency;
				video_frame(middle, mw, mh, mw * sizeof(uint32_t));
				show_current = true;
				between_frames++;
			}
		} else if (show_current) {
			const uint32_t *now;
			unsigned nw, nh;
			if ((now = motion_current(&nw, &nh)))
				video_frame(now, nw, nh, nw * sizeof(uint32_t));
			show_current = false;
		}
		if (run_frame || synced) {
			video_set_overlay(menu_draw(), MENU_WIDTH, MENU_HEIGHT);
			video_present();
		}

		if (show_stats && SDL_GetTicks() - stats_start >= 2000) {
			Uint32 elapsed = SDL_GetTicks() - stats_start;
			printf("tela %d Hz %s | jogo %.2f fps, %.1f ms por quadro | remaster %.1f ms | %ld quadros intermediários (%.1f ms) | áudio %.0f ms na fila, taxa %.4f\n",
				refresh, synced ? "(vsync)" : "(timer)", (frames_run_stats * 1000.0) / elapsed,
				frames_run_stats ? core_ms / frames_run_stats : 0,
				remaster_calls ? remaster_ms / remaster_calls : 0, between_frames,
				between_frames ? between_ms / between_frames : 0,
				SDL_GetQueuedAudioSize(audio) * 1000.0 / (want.freq * 4), audio_ratio);
			between_frames = 0;
			fflush(stdout);
			stats_start = SDL_GetTicks(), frames_run_stats = 0, core_ms = 0, between_ms = 0;
			remaster_ms = 0, remaster_calls = 0;
		}

		if (SDL_GetTicks() - last_save > 30000) {
			save_battery(battery_path);
			if (core.trace_save)
				core.trace_save();
			hdpack_capture_save();
			last_save = SDL_GetTicks();
		}

		if (!synced && !capture_frame)
			SDL_Delay(1);  /* without vsync, poll the clock gently */
	}
	if (ahead.pending) {
		unsigned w, h;
		ahead_finish(&w, &h);
	}

	save_battery(battery_path);
	hdpack_capture_save();
	if (argc == 1)
		save_state(resume_path);
	core.unload_game();
	core.deinit();
	SDL_Quit();
	return 0;
}
