/*
 * keymap.h -- turning key codes into Game Boy buttons.
 *
 * A terminal tells you when a key goes down but never when it comes back
 * up, so a press has to be turned into a timed hold.  Passing the current
 * time in as an argument keeps that arithmetic free of any clock, which is
 * what lets the unit tests drive it deterministically.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

/* How long a single keypress keeps its button down, in milliseconds. */
#define KEYMAP_TAP_MS 250

/* The commands the frontend acts on, as opposed to the buttons that go to
 * the joypad register. */
typedef enum {
    KEYMAP_CMD_NONE = 0,
    KEYMAP_CMD_PAUSE,
    KEYMAP_CMD_RESET,
    KEYMAP_CMD_MUTE,
    KEYMAP_CMD_HELP,
    KEYMAP_CMD_QUIT
} KeymapCommand;

typedef struct {
    uint8_t buttons;          /* GB_KEY_* mask currently held */
    double deadline[8];       /* when each bit stops being held */
    bool help_requested;
} KeyState;

void keymap_init(KeyState *ks);

/* Starts holding whatever `key` means.  Command keys set a pending flag
 * instead; read it with keymap_take_help once per frame. */
void keymap_press(KeyState *ks, PlatformKey key, double now);

/* The button mask to hand the joypad this frame, after letting go of
 * anything whose hold has run out. */
uint8_t keymap_sample(KeyState *ks, double now);

/* Releases everything at once, for instance after a reset. */
void keymap_clear(KeyState *ks);

/* Which Game Boy button bits a key means; zero for a command key. */
uint8_t keymap_button(PlatformKey key);

/* Which command a key means; KEYMAP_CMD_NONE for a button key. */
KeymapCommand keymap_command(PlatformKey key);

/* True, once per press, if the user asked for the controls screen. */
bool keymap_take_help(KeyState *ks);

#endif /* KEYMAP_H */