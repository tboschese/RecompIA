#include "input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PADS 8
#define AXIS_THRESHOLD 16000
#define CAPTURE_AXIS 24000

enum { BIND_NONE, BIND_BUTTON, BIND_AXIS, BIND_HAT };

typedef struct {
	int kind;
	int index;
	int value; /* axis direction (+1/-1) or hat mask */
} Bind;

typedef struct {
	SDL_JoystickID id;
	SDL_GameController *controller; /* recognized gamepad */
	SDL_Joystick *joystick;          /* any gamepad (owned by controller when that is set) */
	Sint16 rest[16];                 /* raw axis values when a capture started */
} Pad;

static const char *const names[BUTTONS] = { "B", "Y", "SELECT", "START", "CIMA", "BAIXO", "ESQ.", "DIR.", "A", "X", "L", "R" };
static const char *const keys_cfg[BUTTONS] = { "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R" };

static SDL_Scancode keys[BUTTONS];
static Bind mapped[BUTTONS]; /* recognized gamepads, SDL's positional layout */
static Bind raw[BUTTONS];    /* other gamepads, raw joystick inputs */

static Pad pads[MAX_PADS];
static int pad_count;
static char config_path[4096];

static bool state[PLAYERS][BUTTONS];
static bool any_now[BUTTONS], any_before[BUTTONS], blocked[BUTTONS];
static bool guide_now, guide_before;
static int capture = -1;

static void defaults(void)
{
	static const SDL_Scancode k[BUTTONS] = {
		[BTN_B] = SDL_SCANCODE_Z, [BTN_A] = SDL_SCANCODE_X, [BTN_Y] = SDL_SCANCODE_A, [BTN_X] = SDL_SCANCODE_S,
		[BTN_L] = SDL_SCANCODE_Q, [BTN_R] = SDL_SCANCODE_W, [BTN_SELECT] = SDL_SCANCODE_RSHIFT,
		[BTN_START] = SDL_SCANCODE_RETURN, [BTN_UP] = SDL_SCANCODE_UP, [BTN_DOWN] = SDL_SCANCODE_DOWN,
		[BTN_LEFT] = SDL_SCANCODE_LEFT, [BTN_RIGHT] = SDL_SCANCODE_RIGHT,
	};
	/* SNES layout by position: SDL's A (south) is the SNES B, SDL's B (east) the SNES A. */
	static const int m[BUTTONS] = {
		[BTN_B] = SDL_CONTROLLER_BUTTON_A, [BTN_A] = SDL_CONTROLLER_BUTTON_B, [BTN_Y] = SDL_CONTROLLER_BUTTON_X,
		[BTN_X] = SDL_CONTROLLER_BUTTON_Y, [BTN_L] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
		[BTN_R] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, [BTN_SELECT] = SDL_CONTROLLER_BUTTON_BACK,
		[BTN_START] = SDL_CONTROLLER_BUTTON_START, [BTN_UP] = SDL_CONTROLLER_BUTTON_DPAD_UP,
		[BTN_DOWN] = SDL_CONTROLLER_BUTTON_DPAD_DOWN, [BTN_LEFT] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
		[BTN_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	};
	/* The usual layout of generic USB SNES-style pads. */
	static const int r[BUTTONS] = {
		[BTN_X] = 0, [BTN_A] = 1, [BTN_B] = 2, [BTN_Y] = 3, [BTN_L] = 4, [BTN_R] = 5,
		[BTN_SELECT] = 8, [BTN_START] = 9,
	};
	static const int hat[BUTTONS] = {
		[BTN_UP] = SDL_HAT_UP, [BTN_DOWN] = SDL_HAT_DOWN, [BTN_LEFT] = SDL_HAT_LEFT, [BTN_RIGHT] = SDL_HAT_RIGHT,
	};
	for (int b = 0; b < BUTTONS; b++) {
		keys[b] = k[b];
		mapped[b] = (Bind){ BIND_BUTTON, m[b], 0 };
		raw[b] = hat[b] ? (Bind){ BIND_HAT, 0, hat[b] } : (Bind){ BIND_BUTTON, r[b], 0 };
	}
}

static void save(void)
{
	FILE *f = fopen(config_path, "w");
	if (!f)
		return;
	for (int b = 0; b < BUTTONS; b++) {
		fprintf(f, "key.%s=%d\n", keys_cfg[b], keys[b]);
		fprintf(f, "pad.%s=%d,%d,%d\n", keys_cfg[b], mapped[b].kind, mapped[b].index, mapped[b].value);
		fprintf(f, "raw.%s=%d,%d,%d\n", keys_cfg[b], raw[b].kind, raw[b].index, raw[b].value);
	}
	fclose(f);
}

static void load(void)
{
	FILE *f = fopen(config_path, "r");
	if (!f)
		return;
	char line[256], group[8], name[16];
	while (fgets(line, sizeof line, f)) {
		int a = 0, b = 0, c = 0;
		int n = sscanf(line, "%7[a-z].%15[A-Z]=%d,%d,%d", group, name, &a, &b, &c);
		if (n < 3)
			continue;
		for (int i = 0; i < BUTTONS; i++) {
			if (strcmp(name, keys_cfg[i]) != 0)
				continue;
			if (!strcmp(group, "key") && a > 0 && a < SDL_NUM_SCANCODES)
				keys[i] = (SDL_Scancode)a;
			else if (!strcmp(group, "pad") && n == 5)
				mapped[i] = (Bind){ a, b, c };
			else if (!strcmp(group, "raw") && n == 5)
				raw[i] = (Bind){ a, b, c };
		}
	}
	fclose(f);
}

void input_init(const char *data_dir, const char *controller_db)
{
	defaults();
	snprintf(config_path, sizeof config_path, "%s/controls.cfg", data_dir);
	load();
	if (controller_db && SDL_GameControllerAddMappingsFromFile(controller_db) < 0)
		fprintf(stderr, "remakeia: %s: %s\n", controller_db, SDL_GetError());
}

static Pad *find_pad(SDL_JoystickID id)
{
	for (int i = 0; i < pad_count; i++)
		if (pads[i].id == id)
			return &pads[i];
	return NULL;
}

static void open_pad(int device, void (*notify)(const char *))
{
	if (pad_count == MAX_PADS || find_pad(SDL_JoystickGetDeviceInstanceID(device)))
		return;
	Pad pad = { 0 };
	if (SDL_IsGameController(device) && (pad.controller = SDL_GameControllerOpen(device)))
		pad.joystick = SDL_GameControllerGetJoystick(pad.controller);
	else if (!(pad.joystick = SDL_JoystickOpen(device)))
		return;
	pad.id = SDL_JoystickInstanceID(pad.joystick);
	pads[pad_count++] = pad;
	if (notify) {
		const char *name = pad.controller ? SDL_GameControllerName(pad.controller) : SDL_JoystickName(pad.joystick);
		char message[128];
		snprintf(message, sizeof message, "Controle %d: %s", pad_count, name ? name : "conectado");
		notify(message);
	}
}

static void close_pad(SDL_JoystickID id, void (*notify)(const char *))
{
	for (int i = 0; i < pad_count; i++) {
		if (pads[i].id != id)
			continue;
		if (pads[i].controller)
			SDL_GameControllerClose(pads[i].controller);
		else
			SDL_JoystickClose(pads[i].joystick);
		memmove(&pads[i], &pads[i + 1], (size_t)(pad_count - i - 1) * sizeof *pads);
		pad_count--;
		if (notify)
			notify("Controle desconectado");
		return;
	}
}

static void captured(void)
{
	capture = -1;
	save();
	input_release_all();
}

bool input_handle_event(const SDL_Event *e, void (*notify)(const char *))
{
	switch (e->type) {
	case SDL_JOYDEVICEADDED:
		open_pad(e->jdevice.which, notify);
		return true;
	case SDL_JOYDEVICEREMOVED:
		close_pad(e->jdevice.which, notify);
		return true;
	}
	if (capture < 0)
		return false;

	Pad *pad;
	switch (e->type) {
	case SDL_KEYDOWN:
		if (e->key.repeat)
			return true;
		if (e->key.keysym.scancode != SDL_SCANCODE_ESCAPE)
			keys[capture] = e->key.keysym.scancode;
		captured();
		return true;
	case SDL_CONTROLLERBUTTONDOWN:
		if (e->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)
			return true;
		mapped[capture] = (Bind){ BIND_BUTTON, e->cbutton.button, 0 };
		captured();
		return true;
	case SDL_CONTROLLERAXISMOTION:
		if (abs(e->caxis.value) < CAPTURE_AXIS)
			return true;
		mapped[capture] = (Bind){ BIND_AXIS, e->caxis.axis, e->caxis.value > 0 ? 1 : -1 };
		captured();
		return true;
	case SDL_JOYBUTTONDOWN:
		if (!(pad = find_pad(e->jbutton.which)) || pad->controller)
			return true;
		raw[capture] = (Bind){ BIND_BUTTON, e->jbutton.button, 0 };
		captured();
		return true;
	case SDL_JOYHATMOTION: {
		int v = e->jhat.value;
		if (!(pad = find_pad(e->jhat.which)) || pad->controller)
			return true;
		if (v != SDL_HAT_UP && v != SDL_HAT_DOWN && v != SDL_HAT_LEFT && v != SDL_HAT_RIGHT)
			return true;
		raw[capture] = (Bind){ BIND_HAT, e->jhat.hat, v };
		captured();
		return true;
	}
	case SDL_JOYAXISMOTION:
		if (!(pad = find_pad(e->jaxis.which)) || pad->controller || e->jaxis.axis >= 16)
			return true;
		/* Triggers rest at one end of their range; only count real movement. */
		if (abs(e->jaxis.value) < CAPTURE_AXIS || abs(pad->rest[e->jaxis.axis]) > 8000)
			return true;
		raw[capture] = (Bind){ BIND_AXIS, e->jaxis.axis, e->jaxis.value > 0 ? 1 : -1 };
		captured();
		return true;
	}
	return false;
}

static bool bound(const Pad *pad, const Bind *bind)
{
	switch (bind->kind) {
	case BIND_BUTTON:
		return pad->controller ? SDL_GameControllerGetButton(pad->controller, bind->index)
							   : SDL_JoystickGetButton(pad->joystick, bind->index);
	case BIND_AXIS: {
		int v = pad->controller ? SDL_GameControllerGetAxis(pad->controller, bind->index)
								: SDL_JoystickGetAxis(pad->joystick, bind->index);
		return v * bind->value > AXIS_THRESHOLD;
	}
	case BIND_HAT:
		return !pad->controller && (SDL_JoystickGetHat(pad->joystick, bind->index) & bind->value);
	}
	return false;
}

static void read_pad(const Pad *pad, bool *out)
{
	const Bind *binds = pad->controller ? mapped : raw;
	for (int b = 0; b < BUTTONS; b++)
		out[b] |= bound(pad, &binds[b]);

	/* The left stick always steers too. */
	int x, y;
	if (pad->controller) {
		x = SDL_GameControllerGetAxis(pad->controller, SDL_CONTROLLER_AXIS_LEFTX);
		y = SDL_GameControllerGetAxis(pad->controller, SDL_CONTROLLER_AXIS_LEFTY);
	} else if (SDL_JoystickNumAxes(pad->joystick) >= 2) {
		x = SDL_JoystickGetAxis(pad->joystick, 0);
		y = SDL_JoystickGetAxis(pad->joystick, 1);
	} else {
		return;
	}
	out[BTN_LEFT] |= x < -AXIS_THRESHOLD;
	out[BTN_RIGHT] |= x > AXIS_THRESHOLD;
	out[BTN_UP] |= y < -AXIS_THRESHOLD;
	out[BTN_DOWN] |= y > AXIS_THRESHOLD;
}

void input_update(void)
{
	memset(state, 0, sizeof state);
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	for (int b = 0; b < BUTTONS; b++)
		state[0][b] = k[keys[b]];
	guide_before = guide_now;
	guide_now = false;
	for (int i = 0; i < pad_count; i++) {
		read_pad(&pads[i], state[i == 0 ? 0 : PLAYERS - 1]);
		if (pads[i].controller)
			guide_now |= SDL_GameControllerGetButton(pads[i].controller, SDL_CONTROLLER_BUTTON_GUIDE);
	}

	for (int b = 0; b < BUTTONS; b++) {
		bool down = false;
		for (int p = 0; p < PLAYERS; p++)
			down |= state[p][b];
		if (!down)
			blocked[b] = false;
		if (blocked[b])
			for (int p = 0; p < PLAYERS; p++)
				state[p][b] = false;
		any_before[b] = any_now[b];
		any_now[b] = down && !blocked[b];
	}
}

bool input_down(int player, int button)
{
	return player >= 0 && player < PLAYERS && button >= 0 && button < BUTTONS && state[player][button];
}

bool input_pressed_any(int button)
{
	return any_now[button] && !any_before[button];
}

bool input_menu_combo(void)
{
	if (guide_now && !guide_before)
		return true;
	return any_now[BTN_SELECT] && any_now[BTN_START] && (input_pressed_any(BTN_SELECT) || input_pressed_any(BTN_START));
}

void input_release_all(void)
{
	for (int b = 0; b < BUTTONS; b++) {
		blocked[b] = true;
		any_now[b] = any_before[b] = false;
	}
	memset(state, 0, sizeof state);
}

bool input_have_pad(void)
{
	return pad_count > 0;
}

void input_capture(int button)
{
	capture = button;
	for (int i = 0; i < pad_count; i++)
		for (int a = 0; a < 16; a++)
			pads[i].rest[a] = a < SDL_JoystickNumAxes(pads[i].joystick) ? SDL_JoystickGetAxis(pads[i].joystick, a) : 0;
}

bool input_capturing(void)
{
	return capture >= 0;
}

void input_reset_defaults(void)
{
	defaults();
	save();
}

const char *input_button_name(int button)
{
	return names[button];
}

static void upper(char *s)
{
	for (; *s; s++)
		*s = (char)toupper((unsigned char)*s);
}

void input_key_label(int button, char *out, size_t size)
{
	switch (keys[button]) {
	case SDL_SCANCODE_UP: snprintf(out, size, "SETA CIMA"); return;
	case SDL_SCANCODE_DOWN: snprintf(out, size, "SETA BAIXO"); return;
	case SDL_SCANCODE_LEFT: snprintf(out, size, "SETA ESQ."); return;
	case SDL_SCANCODE_RIGHT: snprintf(out, size, "SETA DIR."); return;
	case SDL_SCANCODE_RETURN: snprintf(out, size, "ENTER"); return;
	case SDL_SCANCODE_SPACE: snprintf(out, size, "ESPAÇO"); return;
	case SDL_SCANCODE_LSHIFT: snprintf(out, size, "SHIFT ESQ."); return;
	case SDL_SCANCODE_RSHIFT: snprintf(out, size, "SHIFT DIR."); return;
	default: break;
	}
	snprintf(out, size, "%s", SDL_GetScancodeName(keys[button]));
	upper(out);
}

void input_pad_label(int button, char *out, size_t size)
{
	bool recognized = pad_count == 0 || pads[0].controller;
	const Bind *bind = recognized ? &mapped[button] : &raw[button];
	if (bind->kind == BIND_NONE) {
		snprintf(out, size, "-");
	} else if (!recognized) {
		if (bind->kind == BIND_BUTTON)
			snprintf(out, size, "BOTÃO %d", bind->index);
		else if (bind->kind == BIND_AXIS)
			snprintf(out, size, "EIXO %d%c", bind->index, bind->value > 0 ? '+' : '-');
		else
			snprintf(out, size, "D-PAD %s", bind->value == SDL_HAT_UP ? "CIMA" : bind->value == SDL_HAT_DOWN ? "BAIXO"
											  : bind->value == SDL_HAT_LEFT ? "ESQ." : "DIR.");
	} else if (bind->kind == BIND_BUTTON) {
		static const char *const labels[SDL_CONTROLLER_BUTTON_MAX] = {
			[SDL_CONTROLLER_BUTTON_A] = "SUL", [SDL_CONTROLLER_BUTTON_B] = "LESTE",
			[SDL_CONTROLLER_BUTTON_X] = "OESTE", [SDL_CONTROLLER_BUTTON_Y] = "NORTE",
			[SDL_CONTROLLER_BUTTON_BACK] = "SELECT", [SDL_CONTROLLER_BUTTON_START] = "START",
			[SDL_CONTROLLER_BUTTON_LEFTSTICK] = "L3", [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "R3",
			[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "L1", [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "R1",
			[SDL_CONTROLLER_BUTTON_DPAD_UP] = "D-PAD CIMA", [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "D-PAD BAIXO",
			[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "D-PAD ESQ.", [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "D-PAD DIR.",
		};
		const char *label = bind->index >= 0 && bind->index < SDL_CONTROLLER_BUTTON_MAX ? labels[bind->index] : NULL;
		if (label)
			snprintf(out, size, "%s", label);
		else
			snprintf(out, size, "BOTÃO %d", bind->index);
	} else {
		static const char *const axes[SDL_CONTROLLER_AXIS_MAX] = { "ANALOG.E X", "ANALOG.E Y", "ANALOG.D X", "ANALOG.D Y", "L2", "R2" };
		bool trigger = bind->index == SDL_CONTROLLER_AXIS_TRIGGERLEFT || bind->index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
		const char *axis = bind->index >= 0 && bind->index < SDL_CONTROLLER_AXIS_MAX ? axes[bind->index] : "EIXO";
		snprintf(out, size, "%s%s", axis, trigger ? "" : bind->value > 0 ? "+" : "-");
	}
}
