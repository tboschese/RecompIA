/* RemakeIA engine: runs a game project on the desktop (macOS first; Android later through SDL2).
 *
 * Today the SNES hardware comes from a libretro core (snes9x with snesre's tracer); the plan is to
 * replace its CPU, function by function, with the recompiled game code, while this layer grows the
 * enhancements (interpolated motion, layered rendering, widescreen).
 *
 *   engine                              inside an .app: Resources/{core.dylib, game.sfc, game.cfg}
 *   engine CORE ROM DATA_DIR [FRAMES]   development; with FRAMES, runs headless (records the intro)
 *
 * game.cfg has `key=value` lines: `title` names the window and the save folder, every other key is
 * passed to the core as an option (e.g. snes9x_mode7_hires=4x_hv).
 *
 * Saves (cartridge battery, resume point, quick state) and the execution trace live in DATA_DIR, by
 * default ~/Library/Application Support/RemakeIA/<title>/.
 *
 * Keys: arrows, Z=B X=A A=Y S=X Q=L W=R, Enter=Start, Right Shift=Select, Tab (hold)=fast forward,
 * F5=quick save, F7=quick load, F11 or Cmd+F=fullscreen. Gamepads use the SNES layout.
 */

#include <SDL.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_GET_CAN_DUPE 3
#define ENV_GET_SYSTEM_DIRECTORY 9
#define ENV_SET_PIXEL_FORMAT 10
#define ENV_GET_VARIABLE 15
#define ENV_GET_VARIABLE_UPDATE 17
#define ENV_GET_SAVE_DIRECTORY 31
#define ENV_EXPERIMENTAL 0x10000
#define DEVICE_JOYPAD 1
#define MEMORY_SAVE_RAM 0
#define PIXEL_0RGB1555 0
#define PIXEL_XRGB8888 1
#define PIXEL_RGB565 2
#define MAX_OPTIONS 64
#define PATH 4096

enum { B, Y, SELECT, START, UP, DOWN, LEFT, RIGHT, A, X, L, R, BUTTONS };

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
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	void (*trace_save)(void);
} core;

static char title[256] = "RemakeIA";
static char *option_keys[MAX_OPTIONS], *option_values[MAX_OPTIONS];
static int options;
static char data_dir[PATH];
static int pixel_format = PIXEL_0RGB1555;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *frame, *upscaled;
static int frame_w, frame_h, frame_format, upscaled_w, upscaled_h;
static float aspect = 4.0f / 3.0f;
static SDL_AudioDeviceID audio;
static SDL_GameController *pad;
static bool buttons[BUTTONS];
static bool fast_forward;

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

/* Integer nearest-neighbour upscale, then a linear fit to the window: sharp pixels, no shimmer. */
static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data || !renderer)
		return;
	int format = pixel_format == PIXEL_XRGB8888 ? SDL_PIXELFORMAT_ARGB8888
		: pixel_format == PIXEL_RGB565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB1555;
	if (!frame || frame_w != (int)width || frame_h != (int)height || frame_format != format) {
		if (frame)
			SDL_DestroyTexture(frame);
		frame = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING, width, height);
		SDL_SetTextureScaleMode(frame, SDL_ScaleModeNearest);
		frame_w = width, frame_h = height, frame_format = format;
	}
	SDL_UpdateTexture(frame, NULL, data, (int)pitch);

	int w, h;
	SDL_GetRendererOutputSize(renderer, &w, &h);
	SDL_Rect dst = { 0, 0, w, h };
	if (w > h * aspect)
		dst.w = (int)(h * aspect), dst.x = (w - dst.w) / 2;
	else
		dst.h = (int)(w / aspect), dst.y = (h - dst.h) / 2;

	int scale_x = (dst.w + width - 1) / width, scale_y = (dst.h + height - 1) / height;
	int up_w = width * (scale_x > 1 ? scale_x : 1), up_h = height * (scale_y > 1 ? scale_y : 1);
	if (!upscaled || upscaled_w != up_w || upscaled_h != up_h) {
		if (upscaled)
			SDL_DestroyTexture(upscaled);
		upscaled = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, up_w, up_h);
		SDL_SetTextureScaleMode(upscaled, SDL_ScaleModeLinear);
		upscaled_w = up_w, upscaled_h = up_h;
	}
	SDL_SetRenderTarget(renderer, upscaled);
	SDL_RenderCopy(renderer, frame, NULL, NULL);
	SDL_SetRenderTarget(renderer, NULL);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, upscaled, NULL, &dst);
	SDL_RenderPresent(renderer);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
	if (audio && !fast_forward)
		SDL_QueueAudio(audio, data, (Uint32)(frames * 4));
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
	return port == 0 && device == DEVICE_JOYPAD && id < BUTTONS && buttons[id];
}

static void read_input(void)
{
	const Uint8 *key = SDL_GetKeyboardState(NULL);
	static const SDL_Scancode keys[BUTTONS] = {
		[B] = SDL_SCANCODE_Z, [Y] = SDL_SCANCODE_A, [SELECT] = SDL_SCANCODE_RSHIFT, [START] = SDL_SCANCODE_RETURN,
		[UP] = SDL_SCANCODE_UP, [DOWN] = SDL_SCANCODE_DOWN, [LEFT] = SDL_SCANCODE_LEFT, [RIGHT] = SDL_SCANCODE_RIGHT,
		[A] = SDL_SCANCODE_X, [X] = SDL_SCANCODE_S, [L] = SDL_SCANCODE_Q, [R] = SDL_SCANCODE_W,
	};
	static const int pad_buttons[BUTTONS] = {
		[B] = SDL_CONTROLLER_BUTTON_A, [Y] = SDL_CONTROLLER_BUTTON_X, [SELECT] = SDL_CONTROLLER_BUTTON_BACK,
		[START] = SDL_CONTROLLER_BUTTON_START, [UP] = SDL_CONTROLLER_BUTTON_DPAD_UP,
		[DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN, [LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
		[RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT, [A] = SDL_CONTROLLER_BUTTON_B, [X] = SDL_CONTROLLER_BUTTON_Y,
		[L] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER, [R] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
	};
	for (int i = 0; i < BUTTONS; i++)
		buttons[i] = key[keys[i]] || (pad && SDL_GameControllerGetButton(pad, pad_buttons[i]));
	if (pad) {
		const int dead = 16000;
		int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
		int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
		buttons[LEFT] |= x < -dead, buttons[RIGHT] |= x > dead;
		buttons[UP] |= y < -dead, buttons[DOWN] |= y > dead;
		buttons[L] |= SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > dead;
		buttons[R] |= SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > dead;
	}
	fast_forward = key[SDL_SCANCODE_TAB];
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

static void save_state(const char *path)
{
	size_t size = core.serialize_size();
	void *data = malloc(size);
	if (core.serialize(data, size))
		write_file(path, data, size);
	free(data);
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
		else if (options < MAX_OPTIONS)
			option_keys[options] = strdup(line), option_values[options++] = strdup(eq + 1);
	}
	free(text);
}

static void fail(const char *message)
{
	fprintf(stderr, "%s\n", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
	exit(1);
}

#define LOAD(field, name) \
	if (!(*(void **)&core.field = dlsym(lib, name))) fail("núcleo incompleto: falta " name);

int main(int argc, char **argv)
{
	char core_path[PATH], rom_path[PATH];
	long headless_frames = 0;

	if (argc == 1) {
		char *base = SDL_GetBasePath();
		if (!base)
			fail("não achei a pasta de recursos do aplicativo");
		char config[PATH];
		snprintf(config, sizeof config, "%sgame.cfg", base);
		snprintf(core_path, sizeof core_path, "%score.dylib", base);
		snprintf(rom_path, sizeof rom_path, "%sgame.sfc", base);
		read_config(config);
		SDL_free(base);
		char *pref = SDL_GetPrefPath("RemakeIA", title);
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

	void *lib = dlopen(core_path, RTLD_NOW);
	if (!lib)
		fail(dlerror());
	LOAD(init, "retro_init") LOAD(deinit, "retro_deinit") LOAD(set_environment, "retro_set_environment")
	LOAD(set_video_refresh, "retro_set_video_refresh") LOAD(set_audio_sample, "retro_set_audio_sample")
	LOAD(set_audio_sample_batch, "retro_set_audio_sample_batch") LOAD(set_input_poll, "retro_set_input_poll")
	LOAD(set_input_state, "retro_set_input_state") LOAD(get_system_info, "retro_get_system_info")
	LOAD(get_system_av_info, "retro_get_system_av_info") LOAD(load_game, "retro_load_game")
	LOAD(unload_game, "retro_unload_game") LOAD(run, "retro_run") LOAD(serialize_size, "retro_serialize_size")
	LOAD(serialize, "retro_serialize") LOAD(unserialize, "retro_unserialize")
	LOAD(get_memory_data, "retro_get_memory_data") LOAD(get_memory_size, "retro_get_memory_size")
	*(void **)&core.trace_save = dlsym(lib, "snesre_trace_save");

	char battery_path[PATH + 16], resume_path[PATH + 16], quick_path[PATH + 16];
	snprintf(battery_path, sizeof battery_path, "%s/game.srm", data_dir);
	snprintf(resume_path, sizeof resume_path, "%s/resume.state", data_dir);
	snprintf(quick_path, sizeof quick_path, "%s/quick.state", data_dir);

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
	if (av.aspect > 0)
		aspect = av.aspect;
	double fps = av.fps > 0 ? av.fps : 60.0;

	if (headless_frames) {
		for (long i = 0; i < headless_frames; i++) {
			buttons[START] = i % 120 < 4;
			core.run();
		}
		save_battery(battery_path);
		core.unload_game();
		core.deinit();
		SDL_Quit();
		return 0;
	}

	if (argc == 1)
		load_state(resume_path);

	window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		(int)(224 * 3 * aspect), 224 * 3, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
	if (!window || !renderer)
		fail(SDL_GetError());

	SDL_AudioSpec want = { 0 }, have;
	want.freq = av.sample_rate > 0 ? (int)av.sample_rate : 32040;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	SDL_PauseAudioDevice(audio, 0);
	const Uint32 audio_target = (Uint32)(want.freq * 4 / 15);  /* ~67 ms queued */

	Uint64 frequency = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
	Uint32 last_save = SDL_GetTicks();
	bool running = true, paused = false;
	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT)
				running = false;
			else if (event.type == SDL_CONTROLLERDEVICEADDED && !pad)
				pad = SDL_GameControllerOpen(event.cdevice.which);
			else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				paused = true, SDL_PauseAudioDevice(audio, 1);
			else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				paused = false, SDL_PauseAudioDevice(audio, 0), next = SDL_GetPerformanceCounter();
			else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
				SDL_Keycode sym = event.key.keysym.sym;
				if (sym == SDLK_F5)
					save_state(quick_path);
				else if (sym == SDLK_F7)
					load_state(quick_path);
				else if (sym == SDLK_F11 || (sym == SDLK_f && (event.key.keysym.mod & KMOD_GUI))) {
					bool full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
					SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
					SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
				}
			}
		}
		if (paused) {
			SDL_Delay(30);
			continue;
		}
		read_input();
		core.run();

		if (SDL_GetTicks() - last_save > 30000) {
			save_battery(battery_path);
			if (core.trace_save)
				core.trace_save();
			last_save = SDL_GetTicks();
		}

		if (fast_forward) {
			next = SDL_GetPerformanceCounter();
			continue;
		}
		next += (Uint64)(frequency / fps);
		Uint64 now = SDL_GetPerformanceCounter();
		if (SDL_GetQueuedAudioSize(audio) > audio_target && next > now)
			SDL_Delay((Uint32)((next - now) * 1000 / frequency));
		else if (next + frequency / 10 < now)
			next = now;
	}

	save_battery(battery_path);
	if (argc == 1)
		save_state(resume_path);
	core.unload_game();
	core.deinit();
	SDL_Quit();
	return 0;
}
