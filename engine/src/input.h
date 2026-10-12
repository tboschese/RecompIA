#ifndef REMASTERIA_INPUT_H
#define REMASTERIA_INPUT_H

#include <SDL.h>
#include <stdbool.h>

/* SNES buttons, in libretro joypad order. */
enum { BTN_B, BTN_Y, BTN_SELECT, BTN_START, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_X, BTN_L, BTN_R, BUTTONS };
#define PLAYERS 2

/* Keyboard and gamepads (USB/Bluetooth), hot-plugged. The keyboard and the first gamepad drive
 * player 1, the second gamepad player 2. Gamepads SDL recognizes (built-in list plus
 * gamecontrollerdb.txt) use the SNES layout by position; any other gamepad works after remapping.
 * Bindings live in DATA_DIR/controls.cfg. */
void input_init(const char *data_dir, const char *controller_db);
/* True when the event was used up (a remap capture or a hot-plug). */
bool input_handle_event(const SDL_Event *event, void (*notify)(const char *message));
void input_update(void);                 /* once per frame, before reading */
bool input_down(int player, int button);
bool input_pressed_any(int button);      /* went down this frame, on any device */
bool input_menu_combo(void);             /* Select+Start together, or the gamepad's Home button */
void input_release_all(void);            /* ignore held buttons until they are released */
bool input_have_pad(void);

/* Remapping: after input_capture(button), the next key goes to the keyboard binding and the next
 * gamepad button/direction to the gamepad binding. */
void input_capture(int button);
bool input_capturing(void);
void input_reset_defaults(void);
const char *input_button_name(int button);
void input_key_label(int button, char *out, size_t size);
void input_pad_label(int button, char *out, size_t size);

#endif
