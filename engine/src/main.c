/* RemakeIA engine: runs a game project on the desktop (macOS first; Android later through SDL2).
 *
 * Today the SNES hardware comes from a libretro core (snes9x with snesre's tracer); the plan is to
 * replace its CPU, function by function, with the recompiled game code, while this layer grows the
 * enhancements (interpolated motion, layered rendering, widescreen).
 *
 *   engine                              inside an .app: Resources/{core.dylib, game.sfc, game.cfg}
 *   engine CORE ROM DATA_DIR [FRAMES]   development; with FRAMES, runs headless (records the intro)
 *
 * game.cfg has `key=value` lines: `title` names the window and the save folder, `video` picks the
 * look (pixel, crt; see video.c), every other key is passed to the core as an option
 * (e.g. snes9x_mode7_hires=4x_hv). Shaders come from Resources/shaders, or $REMAKEIA_SHADERS.
 *
 * Saves (cartridge battery, resume point, quick state) and the execution trace live in DATA_DIR, by
 * default ~/Library/Application Support/RemakeIA/<title>/.
 *
 * Keys: arrows, Z=B X=A A=Y S=X Q=L W=R, Enter=Start, Right Shift=Select, Tab (hold)=fast forward,
 * F2=next look, F5=quick save, F7=quick load, F11 or Cmd+F=fullscreen. Gamepads use the SNES layout.
 */

#include <SDL.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static char look[32] = "pixel";
static char *option_keys[MAX_OPTIONS], *option_values[MAX_OPTIONS];
static int options;
static char data_dir[PATH];

static SDL_Window *window;
static bool video_ready;
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
		video_set_format(*(const int *)data);
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

static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (data && video_ready)
		video_frame(data, width, height, pitch);
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
		else if (!strcmp(line, "video"))
			snprintf(look, sizeof look, "%s", eq + 1);
		else if (options < MAX_OPTIONS)
			option_keys[options] = strdup(line), option_values[options++] = strdup(eq + 1);
	}
	free(text);
}

/* Automated visual checks: REMAKEIA_CAPTURE=out.ppm REMAKEIA_CAPTURE_FRAME=N saves frame N and quits. */
static bool capture_mode(void)
{
	return getenv("REMAKEIA_CAPTURE") && getenv("REMAKEIA_CAPTURE_FRAME");
}

static void update_title(void)
{
	char text[512];
	snprintf(text, sizeof text, "%s — %s", title, video_profile_name());
	SDL_SetWindowTitle(window, text);
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
	char core_path[PATH], rom_path[PATH], shader_dir[PATH] = "";
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
	if (argc != 1 && getenv("REMAKEIA_CONFIG"))
		read_config(getenv("REMAKEIA_CONFIG"));
	if (getenv("REMAKEIA_VIDEO"))
		snprintf(look, sizeof look, "%s", getenv("REMAKEIA_VIDEO"));
	if (getenv("REMAKEIA_SHADERS"))
		snprintf(shader_dir, sizeof shader_dir, "%s", getenv("REMAKEIA_SHADERS"));

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
	if (av.base_width && av.base_height)
		aspect = (float)av.base_width / av.base_height;  /* square pixels */
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

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		(int)(224 * 3 * aspect), 224 * 3, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL);
	if (!window)
		fail(SDL_GetError());
	video_ready = video_init(window, shader_dir[0] ? shader_dir : NULL, look);
	if (!video_ready)
		fail("não consegui iniciar o vídeo (OpenGL e shaders)");
	update_title();

	SDL_AudioSpec want = { 0 }, have;
	want.freq = av.sample_rate > 0 ? (int)av.sample_rate : 32040;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	audio_target_bytes = (Uint32)(want.freq * 4 / 20);  /* 50 ms queued */

	/* Frame pacing: lock the game to the display (vsync) when its refresh is a multiple of the
	 * game's rate (60 Hz, 120 Hz ProMotion...), otherwise fall back to a timer. */
	SDL_DisplayMode mode;
	int refresh = SDL_GetWindowDisplayMode(window, &mode) == 0 && mode.refresh_rate > 0 ? mode.refresh_rate : 60;
	int refreshes_per_frame = (int)(refresh / fps + 0.5);
	if (refreshes_per_frame < 1)
		refreshes_per_frame = 1;
	double locked_rate = (double)refresh / refreshes_per_frame;
	bool synced = !capture_mode() && locked_rate / fps > 0.98 && locked_rate / fps < 1.02 && video_set_vsync(true);
	/* Locked to the display, the game runs at its refresh rate (60.0 instead of 60.1 Hz): every
	 * frame then lands on a refresh, with no periodic double step; audio rate control absorbs it. */
	double rate = synced ? locked_rate : fps;

	Uint64 frequency = SDL_GetPerformanceFrequency();
	Uint64 clock_start = SDL_GetPerformanceCounter() - frequency / (Uint64)(2 * rate);  /* half-frame phase */
	long frames_done = 0;
	double core_ms = 0;
	Uint32 last_save = SDL_GetTicks();
	bool running = true, paused = false;
	long capture_frame = capture_mode() ? atol(getenv("REMAKEIA_CAPTURE_FRAME")) : 0;
	long frames_run = 0;
	bool show_stats = getenv("REMAKEIA_STATS") != NULL;  /* pacing diagnostics on stdout */
	Uint32 stats_start = SDL_GetTicks();
	long frames_run_stats = 0;
	while (running) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT)
				running = false;
			else if (event.type == SDL_CONTROLLERDEVICEADDED && !pad)
				pad = SDL_GameControllerOpen(event.cdevice.which);
			else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !capture_frame)
				paused = true, SDL_PauseAudioDevice(audio, 1);
			else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				paused = false, clock_start = SDL_GetPerformanceCounter(), frames_done = 0;
			else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
				SDL_Keycode sym = event.key.keysym.sym;
				if (sym == SDLK_F2) {
					video_next_profile();
					update_title();
				} else if (sym == SDLK_F5)
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
		long due = capture_frame ? 1
			: (long)((SDL_GetPerformanceCounter() - clock_start) * rate / frequency) - frames_done;
		if (due > 3) {  /* fell far behind (window dragged, system busy): resync instead of rushing */
			clock_start = SDL_GetPerformanceCounter() - frequency / (Uint64)(2 * rate);
			frames_done = 0;
			due = 1;
		}
		bool run_frame = due > 0;
		for (long f = 0; f < due; f++) {
			Uint64 t0 = SDL_GetPerformanceCounter();
			for (int i = 0; i < (fast_forward ? FAST_FORWARD_FRAMES : 1); i++)
				core.run();
			core_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / frequency;
			frames_done++;
			frames_run_stats++;
			update_audio_ratio();
			if (capture_frame && ++frames_run == capture_frame) {
				video_capture_next(getenv("REMAKEIA_CAPTURE"));
				running = false;
			}
		}
		if (run_frame || synced)
			video_present();

		if (show_stats && SDL_GetTicks() - stats_start >= 2000) {
			Uint32 elapsed = SDL_GetTicks() - stats_start;
			printf("tela %d Hz %s | jogo %.2f fps, %.1f ms por quadro | áudio %.0f ms na fila, taxa %.4f\n", refresh,
				synced ? "(vsync)" : "(timer)", (frames_run_stats * 1000.0) / elapsed,
				frames_run_stats ? core_ms / frames_run_stats : 0, SDL_GetQueuedAudioSize(audio) * 1000.0 / (want.freq * 4),
				audio_ratio);
			fflush(stdout);
			stats_start = SDL_GetTicks(), frames_run_stats = 0, core_ms = 0;
		}

		if (SDL_GetTicks() - last_save > 30000) {
			save_battery(battery_path);
			if (core.trace_save)
				core.trace_save();
			last_save = SDL_GetTicks();
		}

		if (!synced && !capture_frame)
			SDL_Delay(1);  /* without vsync, poll the clock gently */
	}

	save_battery(battery_path);
	if (argc == 1)
		save_state(resume_path);
	core.unload_game();
	core.deinit();
	SDL_Quit();
	return 0;
}
