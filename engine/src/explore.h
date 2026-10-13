#ifndef REMASTERIA_EXPLORE_H
#define REMASTERIA_EXPLORE_H

#include <stdbool.h>
#include <stdint.h>

#include "motion.h"

/* Sweep (tools/remasteria sweep): the engine plays alone from a state with random input, the
 * way a fuzzer does, and keeps a state and a picture whenever the screen shows tiles not seen
 * before. A tile is the key the HD packs use (its graphics and palette), so what is found is
 * what a light list or an art pack names. The tool runs many of these from the states that
 * found the most, until nothing new turns up.
 *
 * dir gets s<frame>.state, s<frame>.ppm and keys.tsv: key picture x y layer shape, one line per
 * new tile (layer 4 is a sprite). shape is its graphics alone (hdpack_shape) when those are new
 * too, else 0: a fade or a flash makes new keys, not new graphics, and only new graphics lead the
 * sweep. known is a file of keys and shapes already found, one hex number a line. */
void explore_init(const char *dir, const char *known, unsigned seed);
bool explore_on(void);
/* The buttons held on this frame, one bit per BTN_ id. */
unsigned explore_buttons(long frame);
/* Every frame the core draws. True when the frame shows enough new tiles to keep. */
bool explore_frame(const CoreMotion *m, unsigned height);
/* After the state and picture of the frame are saved under `name`. */
void explore_keep(const char *name);
/* New tiles not kept yet, at the end of the run. */
bool explore_pending(void);

#endif
