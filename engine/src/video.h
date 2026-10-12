#ifndef REMASTERIA_VIDEO_H
#define REMASTERIA_VIDEO_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { VIDEO_0RGB1555, VIDEO_XRGB8888, VIDEO_RGB565 };

/* Creates the OpenGL context on `window` and loads the shaders from `shader_dir`.
 * `profile` is "pixel" or "crt"; unknown names or missing shaders fall back to "pixel". */
bool video_init(SDL_Window *window, const char *shader_dir, const char *profile);
void video_set_format(int format);  /* libretro pixel format of the frames that follow */
/* Receives a new game frame; video_present() draws the latest one and swaps (waits for vsync if on). */
void video_frame(const void *data, unsigned width, unsigned height, size_t pitch);
void video_present(void);
/* RGBA image (bytes in memory order) blended over the game at the same scale; NULL hides it. */
void video_set_overlay(const uint32_t *rgba, int width, int height);
bool video_set_vsync(bool on);
void video_next_profile(void);
const char *video_profile_name(void);
/* Saves the next frame drawn, as it appears on screen, to a binary PPM (automated visual checks). */
void video_capture_next(const char *path);

#endif
