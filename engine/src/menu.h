#ifndef REMASTERIA_MENU_H
#define REMASTERIA_MENU_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define MENU_WIDTH 256
#define MENU_HEIGHT 224
#define SLOTS 8
#define THUMB_WIDTH 64
#define THUMB_HEIGHT 56

/* Pixels are RGBA bytes in memory. */
static inline uint32_t menu_rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{
	return r | g << 8 | b << 16 | (uint32_t)a << 24;
}

typedef struct {
	bool (*save)(int slot);
	bool (*load)(int slot);
	/* False when the slot is empty; otherwise fills the date and the RGBA thumbnail. */
	bool (*slot_info)(int slot, char *when, size_t size, uint32_t *thumb);
	void (*fullscreen)(void);
	void (*next_look)(void);
	const char *(*look_name)(void);
	void (*next_art)(void);
	const char *(*art_name)(void);
	void (*next_motion)(void);
	const char *(*motion_name)(void);
	void (*quit)(void);
} MenuActions;

void menu_init(const MenuActions *actions, const char *title);
void menu_open(void);
void menu_close(void);
void menu_show(const char *screen); /* main, save, load or controls (automated captures) */
bool menu_is_open(void);
bool menu_handle_event(const SDL_Event *event); /* true when used */
void menu_update(void);                         /* once per frame while open, after input_update */
void menu_toast(const char *message);
int menu_slot(void);                            /* last slot used, for the quick save keys */
void menu_set_slot(int slot);
/* The overlay to draw over the game, or NULL when there is nothing to show. */
const uint32_t *menu_draw(void);

#endif
