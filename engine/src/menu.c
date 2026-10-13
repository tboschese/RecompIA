#include "menu.h"

#include "input.h"
#include "third_party/font8x8_basic.h"
#include "third_party/font8x8_ext_latin.h"

#include <stdio.h>
#include <string.h>

#define TOAST_MS 2500
#define REPEAT_DELAY 18
#define REPEAT_RATE 5

enum { SCREEN_MAIN, SCREEN_SAVE, SCREEN_LOAD, SCREEN_CONTROLS };
enum { ITEM_CONTINUE, ITEM_SAVE, ITEM_LOAD, ITEM_CONTROLS, ITEM_LOOK, ITEM_MOTION, ITEM_FULLSCREEN, ITEM_RESET, ITEM_QUIT, MAIN_ITEMS };
#define CONTROL_ITEMS (BUTTONS + 2)

typedef struct {
	bool used;
	char when[32];
	uint32_t thumb[THUMB_WIDTH * THUMB_HEIGHT];
} Slot;

static MenuActions act;
static char title[64];
static uint32_t canvas[MENU_WIDTH * MENU_HEIGHT];
static bool open;
static int screen, cursor[4], slot;
static Slot slots[SLOTS];
static char toast[128];
static Uint32 toast_until;
static int held, held_frames;
static bool reset_armed;

static const uint32_t TEXT = 0xFFF0F0F0, DIM = 0xFF9090A0, GOLD = 0xFF40D8FF, SHADOW = 0xC0000000;

static void fill(int x, int y, int w, int h, uint32_t color)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			if (i >= 0 && j >= 0 && i < MENU_WIDTH && j < MENU_HEIGHT)
				canvas[j * MENU_WIDTH + i] = color;
}

static const unsigned char *glyph(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	unsigned code = *p++;
	if (code >= 0xC0 && (*p & 0xC0) == 0x80)
		code = (code & 0x1F) << 6 | (*p++ & 0x3F);
	*s = (const char *)p;
	if (code < 128)
		return (const unsigned char *)font8x8_basic[code];
	if (code >= 0xA0 && code <= 0xFF)
		return (const unsigned char *)font8x8_ext_latin[code - 0xA0];
	return (const unsigned char *)font8x8_basic['?'];
}

static int text_width(const char *s)
{
	int n = 0;
	while (*s) {
		glyph(&s);
		n++;
	}
	return n * 8;
}

static void text(int x, int y, const char *s, uint32_t color)
{
	while (*s) {
		const unsigned char *g = glyph(&s);
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++)
				if (g[j] >> i & 1) {
					fill(x + i + 1, y + j + 1, 1, 1, SHADOW);
					fill(x + i, y + j, 1, 1, color);
				}
		x += 8;
	}
}

static void centered(int y, const char *s, uint32_t color)
{
	text((MENU_WIDTH - text_width(s)) / 2, y, s, color);
}

static void frame(int x, int y, int w, int h)
{
	fill(x, y, w, h, 0xE0301810);
	fill(x, y, w, 1, 0xFFD0B090);
	fill(x, y + h - 1, w, 1, 0xFF806050);
	fill(x, y, 1, h, 0xFFD0B090);
	fill(x + w - 1, y, 1, h, 0xFF806050);
}

static void refresh_slots(void)
{
	for (int i = 0; i < SLOTS; i++)
		slots[i].used = act.slot_info(i + 1, slots[i].when, sizeof slots[i].when, slots[i].thumb);
}

void menu_init(const MenuActions *actions, const char *name)
{
	act = *actions;
	snprintf(title, sizeof title, "%s", name);
	slot = 1;
}

void menu_open(void)
{
	open = true;
	screen = SCREEN_MAIN;
	cursor[SCREEN_MAIN] = ITEM_CONTINUE;
	reset_armed = false;
	input_release_all();
}

void menu_show(const char *name)
{
	menu_open();
	if (!strcmp(name, "save") || !strcmp(name, "load")) {
		screen = name[0] == 's' ? SCREEN_SAVE : SCREEN_LOAD;
		cursor[screen] = slot - 1;
		refresh_slots();
	} else if (!strcmp(name, "controls")) {
		screen = SCREEN_CONTROLS;
	}
}

void menu_close(void)
{
	open = false;
	input_release_all();
}

bool menu_is_open(void)
{
	return open;
}

void menu_toast(const char *message)
{
	snprintf(toast, sizeof toast, "%s", message);
	toast_until = SDL_GetTicks() + TOAST_MS;
}

int menu_slot(void)
{
	return slot;
}

void menu_set_slot(int n)
{
	slot = n;
}

static void back(void)
{
	if (screen == SCREEN_MAIN)
		menu_close();
	else
		screen = SCREEN_MAIN;
}

bool menu_handle_event(const SDL_Event *e)
{
	if (e->type != SDL_KEYDOWN || e->key.repeat)
		return false;
	SDL_Scancode key = e->key.keysym.scancode;
	if (key == SDL_SCANCODE_ESCAPE) {
		if (open)
			back();
		else
			menu_open();
		return true;
	}
	if (open && key == SDL_SCANCODE_BACKSPACE) {
		back();
		return true;
	}
	return false;
}

/* A direction that fires once on press, then repeats while held. */
static bool step(int button)
{
	if (input_pressed_any(button)) {
		held = button;
		held_frames = 0;
		return true;
	}
	if (held != button)
		return false;
	if (!input_down(0, button) && !input_down(PLAYERS - 1, button)) {
		held = -1;
		return false;
	}
	held_frames++;
	return held_frames > REPEAT_DELAY && (held_frames - REPEAT_DELAY) % REPEAT_RATE == 0;
}

static int items(void)
{
	switch (screen) {
	case SCREEN_MAIN: return MAIN_ITEMS;
	case SCREEN_CONTROLS: return CONTROL_ITEMS;
	default: return SLOTS;
	}
}

static void choose(void)
{
	int c = cursor[screen];
	char message[64];
	switch (screen) {
	case SCREEN_MAIN:
		switch (c) {
		case ITEM_CONTINUE: menu_close(); break;
		case ITEM_SAVE:
		case ITEM_LOAD:
			screen = c == ITEM_SAVE ? SCREEN_SAVE : SCREEN_LOAD;
			cursor[screen] = slot - 1;
			refresh_slots();
			break;
		case ITEM_CONTROLS: screen = SCREEN_CONTROLS; break;
		case ITEM_LOOK: act.next_look(); break;
		case ITEM_MOTION: act.next_motion(); break;
		case ITEM_FULLSCREEN: act.fullscreen(); break;
		case ITEM_RESET:
			/* A second press within the warning confirms: progress since the last save is lost. */
			if (reset_armed && SDL_GetTicks() < toast_until) {
				reset_armed = false;
				act.reset();
				menu_close();
				menu_toast("Jogo reiniciado");
			} else {
				reset_armed = true;
				menu_toast("Aperte de novo para reiniciar");
			}
			break;
		case ITEM_QUIT: act.quit(); break;
		}
		break;
	case SCREEN_SAVE:
		slot = c + 1;
		if (act.save(slot)) {
			snprintf(message, sizeof message, "Estado salvo no slot %d", slot);
			menu_toast(message);
			refresh_slots();
		} else {
			menu_toast("Não foi possível salvar");
		}
		break;
	case SCREEN_LOAD:
		if (!slots[c].used) {
			menu_toast("Slot vazio");
		} else if (act.load(c + 1)) {
			slot = c + 1;
			snprintf(message, sizeof message, "Estado do slot %d carregado", slot);
			menu_toast(message);
			menu_close();
		} else {
			menu_toast("Não foi possível carregar");
		}
		break;
	case SCREEN_CONTROLS:
		if (c < BUTTONS) {
			input_capture(c);
		} else if (c == BUTTONS) {
			input_reset_defaults();
			menu_toast("Controles restaurados");
		} else {
			screen = SCREEN_MAIN;
		}
		break;
	}
}

void menu_update(void)
{
	if (!open || input_capturing())
		return;
	int n = items();
	if (step(BTN_UP))
		cursor[screen] = (cursor[screen] + n - 1) % n, reset_armed = false;
	if (step(BTN_DOWN))
		cursor[screen] = (cursor[screen] + 1) % n, reset_armed = false;
	if (input_pressed_any(BTN_A) || input_pressed_any(BTN_START))
		choose();
	else if (input_pressed_any(BTN_B))
		back();
}

static void draw_main(void)
{
	frame(24, 40, 208, 16 + MAIN_ITEMS * 14);
	char look[48], motion[48];
	snprintf(look, sizeof look, "Visual: %s", act.look_name());
	snprintf(motion, sizeof motion, "Movimento: %s", act.motion_name());
	const char *labels[MAIN_ITEMS] = { "Continuar", "Salvar estado", "Carregar estado", "Controles", look, motion, "Tela cheia", "Reiniciar jogo", "Sair do jogo" };
	for (int i = 0; i < MAIN_ITEMS; i++) {
		bool on = i == cursor[SCREEN_MAIN];
		if (on)
			text(34, 52 + i * 14, ">", GOLD);
		text(48, 52 + i * 14, labels[i], on ? GOLD : TEXT);
	}
}

static void draw_slots(void)
{
	centered(28, screen == SCREEN_SAVE ? "Salvar estado" : "Carregar estado", GOLD);
	frame(8, 44, 240, 24 + SLOTS * 14);
	for (int i = 0; i < SLOTS; i++) {
		bool on = i == cursor[screen];
		char line[48];
		snprintf(line, sizeof line, "%d %s", i + 1, slots[i].used ? slots[i].when : "vazio");
		if (on)
			text(14, 56 + i * 14, ">", GOLD);
		text(26, 56 + i * 14, line, on ? GOLD : slots[i].used ? TEXT : DIM);
	}
	const Slot *s = &slots[cursor[screen]];
	int x = 172, y = 56;
	fill(x - 2, y - 2, THUMB_WIDTH + 4, THUMB_HEIGHT + 4, 0xFFD0B090);
	if (s->used)
		for (int j = 0; j < THUMB_HEIGHT; j++)
			memcpy(&canvas[(y + j) * MENU_WIDTH + x], &s->thumb[j * THUMB_WIDTH], THUMB_WIDTH * sizeof(uint32_t));
	else
		fill(x, y, THUMB_WIDTH, THUMB_HEIGHT, 0xFF000000);
	centered(204, screen == SCREEN_SAVE ? "A: salvar   B: voltar" : "A: carregar   B: voltar", DIM);
}

static void clip(char *s, int chars)
{
	const char *p = s;
	for (int n = 0; *p && n < chars; n++)
		glyph(&p);
	s[p - s] = 0;
}

static void draw_controls(void)
{
	centered(6, "Controles", GOLD);
	frame(4, 18, 248, 22 + CONTROL_ITEMS * 12);
	text(20, 24, "SNES", DIM);
	text(76, 24, "Teclado", DIM);
	text(160, 24, input_have_pad() ? "Controle" : "(controle)", DIM);
	for (int i = 0; i < CONTROL_ITEMS; i++) {
		bool on = i == cursor[SCREEN_CONTROLS];
		int y = 38 + i * 12;
		uint32_t color = on ? GOLD : TEXT;
		if (on)
			text(8, y, ">", GOLD);
		if (i == BUTTONS) {
			text(20, y, "Restaurar padrão", color);
			continue;
		}
		if (i == BUTTONS + 1) {
			text(20, y, "Voltar", color);
			continue;
		}
		char key[48], pad[48];
		text(20, y, input_button_name(i), color);
		if (on && input_capturing()) {
			text(76, y, "Aperte tecla ou botão", GOLD);
			continue;
		}
		input_key_label(i, key, sizeof key);
		input_pad_label(i, pad, sizeof pad);
		clip(key, 10);
		clip(pad, 11);
		text(76, y, key, color);
		text(160, y, pad, color);
	}
	centered(210, input_capturing() ? "Esc: cancelar" : "A: trocar   B: voltar", DIM);
}

const uint32_t *menu_draw(void)
{
	bool toasting = SDL_GetTicks() < toast_until;
	if (!open && !toasting)
		return NULL;
	memset(canvas, 0, sizeof canvas);
	if (open) {
		fill(0, 0, MENU_WIDTH, MENU_HEIGHT, 0xA0000000);
		if (screen == SCREEN_MAIN)
			centered(20, title, GOLD);
		switch (screen) {
		case SCREEN_MAIN: draw_main(); break;
		case SCREEN_SAVE:
		case SCREEN_LOAD: draw_slots(); break;
		case SCREEN_CONTROLS: draw_controls(); break;
		}
	}
	if (toasting) {
		int w = text_width(toast) + 16;
		if (w > MENU_WIDTH)
			w = MENU_WIDTH;
		int y = open ? 186 : 200;
		if (open && screen == SCREEN_CONTROLS)
			y = 196;
		frame((MENU_WIDTH - w) / 2, y, w, 16);
		centered(y + 4, toast, TEXT);
	}
	return canvas;
}
