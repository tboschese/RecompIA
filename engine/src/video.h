#ifndef REMASTERIA_VIDEO_H
#define REMASTERIA_VIDEO_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diorama.h"

enum { VIDEO_0RGB1555, VIDEO_XRGB8888, VIDEO_RGB565 };

/* Creates the OpenGL context on `window` and loads the shaders from `shader_dir`.
 * `profile` is "pixel" or "crt"; unknown names or missing shaders fall back to "pixel". */
bool video_init(SDL_Window *window, const char *shader_dir, const char *profile);
/* Largest integer scale of a 256×224 frame that fits the drawable. Capture stays at the
 * nominal grid so a recorded frame keeps one size. */
int video_fit_scale(void);
void video_set_format(int format);  /* libretro pixel format of the frames that follow */
/* Receives a new game frame; video_present() draws the latest one and swaps (waits for vsync if on). */
void video_frame(const void *data, unsigned width, unsigned height, size_t pitch);
void video_present(void);
/* Draws the current frame and waits for the GPU, without putting it on screen. */
void video_finish(void);
/* Draws the current frame into an offscreen target, so the window stays as it was. */
void video_preload(void);
/* Clears the window to black and swaps. Used while the picture is not ready to show. */
void video_blank(void);
/* Drops the uploaded picture so the next present stays blank until a new frame arrives. */
void video_forget(void);
/* RGBA image (bytes in memory order) blended over the game at the same scale; NULL hides it. */
void video_set_overlay(const uint32_t *rgba, int width, int height);
/* Diorama look (diorama.h): the layers of the picture that follows, NULL when it has none (it is
 * then drawn pixel perfect). The phase moves the next present halfway back towards the frame
 * before, for the 120 Hz in-between; it goes back to 0 after that present. */
bool video_diorama_on(void);
void video_diorama(const Diorama *layers);
void video_diorama_phase(float phase);
bool video_set_vsync(bool on);
void video_next_profile(void);
const char *video_profile_name(void);
/* Saves the next frame drawn, as it appears on screen, to a binary PPM (automated visual checks). */
void video_capture_next(const char *path);
/* A Mac display in a "scaled" mode draws everything larger and shrinks it, smoothing every pixel.
 * While the game runs, in a window or fullscreen, the display at (x, y) in desktop points runs at
 * its panel's own pixels; macOS goes back by itself when the app quits. */
void video_native_display(int x, int y);

#endif
