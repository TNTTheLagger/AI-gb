/*
 * keymap.c -- the Game Boy side of the keyboard.
 *
 * Maps a key code to either a joypad bit or a frontend command, and
 * stretches each press into a short hold so that a terminal, which reports
 * presses but never releases, still produces usable input.
 */
#include "keymap.h"

#include <string.h>

#include "gb.h"

uint8_t keymap_button(PlatformKey key)
{
    switch (key) {
    case PLATFORM_KEY_UP:     return GB_KEY_UP;
    case PLATFORM_KEY_DOWN:   return GB_KEY_DOWN;
    case PLATFORM_KEY_LEFT:   return GB_KEY_LEFT;
    case PLATFORM_KEY_RIGHT:  return GB_KEY_RIGHT;
    case PLATFORM_KEY_A:      return GB_KEY_A;
    case PLATFORM_KEY_B:      return GB_KEY_B;
    case PLATFORM_KEY_START:  return GB_KEY_START;
    case PLATFORM_KEY_SELECT: return GB_KEY_SELECT;
    default:                  return 0;
    }
}

KeymapCommand keymap_command(PlatformKey key)
{
    switch (key) {
    case PLATFORM_KEY_PAUSE: return KEYMAP_CMD_PAUSE;
    case PLATFORM_KEY_RESET: return KEYMAP_CMD_RESET;
    case PLATFORM_KEY_MUTE:  return KEYMAP_CMD_MUTE;
    case PLATFORM_KEY_HELP:  return KEYMAP_CMD_HELP;
    case PLATFORM_KEY_QUIT:  return KEYMAP_CMD_QUIT;
    default:                 return KEYMAP_CMD_NONE;
    }
}

void keymap_init(KeyState *ks)
{
    memset(ks, 0, sizeof(*ks));
}

void keymap_clear(KeyState *ks)
{
    ks->buttons = 0;
    memset(ks->deadline, 0, sizeof(ks->deadline));
}

void keymap_press(KeyState *ks, PlatformKey key, double now)
{
    uint8_t button = keymap_button(key);

    if (button) {
        for (int i = 0; i < 8; i++) {
            if (button & (1u << i))
                ks->deadline[i] = now + KEYMAP_TAP_MS / 1000.0;
        }
        ks->buttons |= button;
        return;
    }

    if (key == PLATFORM_KEY_HELP)
        ks->help_requested = true;
}

uint8_t keymap_sample(KeyState *ks, double now)
{
    for (int i = 0; i < 8; i++) {
        if ((ks->buttons & (1u << i)) && now > ks->deadline[i])
            ks->buttons &= (uint8_t)~(1u << i);
    }
    return ks->buttons;
}

bool keymap_take_help(KeyState *ks)
{
    bool wanted = ks->help_requested;

    ks->help_requested = false;
    return wanted;
}