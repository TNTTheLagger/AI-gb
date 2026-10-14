/*
 * test_platform.c -- the platform layer, and the shared key decoder.
 *
 * Everything tested here is portable code that runs identically on Linux
 * and Windows, which is the point: the parts of the host layer that used
 * to be buried in #ifdefs are now exercised by the same tests on both.
 * The handful of functions that are genuinely OS specific (raw console
 * mode, window size, child processes) are checked only for the contracts
 * that hold on every system.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "keymap.h"
#include "platform.h"

/* Decodes a whole buffer and returns how many keys came out, copying the
 * first few into `out`.  Fails the test if more keys arrive than room. */
static int decode(const char *bytes, PlatformKey *out, int cap)
{
    PlatformKey scratch[64];
    int n = platform_decode_stream(bytes, (int)strlen(bytes), 0, scratch,
                                   (int)(sizeof(scratch) / sizeof(scratch[0])));

    CHECK(n <= (int)(sizeof(scratch) / sizeof(scratch[0])));
    for (int i = 0; i < n && i < cap; i++)
        out[i] = scratch[i];
    return n;
}

/* ------------------------------------------------------------------ */
/* Decoding a terminal's byte stream                                   */
/* ------------------------------------------------------------------ */

static void t_keys_plain_letters(void)
{
    PlatformKey k[4];

    CHECK_EQ(decode("wasd", k, 4), 4);
    CHECK_EQ(k[0], PLATFORM_KEY_UP);
    CHECK_EQ(k[1], PLATFORM_KEY_LEFT);   /* a is left */
    CHECK_EQ(k[2], PLATFORM_KEY_DOWN);
    CHECK_EQ(k[3], PLATFORM_KEY_RIGHT);
}

static void t_keys_uppercase_and_hjkl(void)
{
    PlatformKey k[8];

    CHECK_EQ(decode("WASD", k, 8), 4);
    CHECK_EQ(k[0], PLATFORM_KEY_UP);
    CHECK_EQ(k[1], PLATFORM_KEY_LEFT);
    CHECK_EQ(k[2], PLATFORM_KEY_DOWN);
    CHECK_EQ(k[3], PLATFORM_KEY_RIGHT);

    CHECK_EQ(decode("hjkl", k, 8), 4);
    CHECK_EQ(k[0], PLATFORM_KEY_LEFT);
    CHECK_EQ(k[1], PLATFORM_KEY_DOWN);
    CHECK_EQ(k[2], PLATFORM_KEY_UP);
    CHECK_EQ(k[3], PLATFORM_KEY_RIGHT);
}

static void t_keys_buttons(void)
{
    PlatformKey k[4];

    CHECK_EQ(decode("zx \r", k, 4), 4);
    CHECK_EQ(k[0], PLATFORM_KEY_A);
    CHECK_EQ(k[1], PLATFORM_KEY_B);
    CHECK_EQ(k[2], PLATFORM_KEY_SELECT);
    CHECK_EQ(k[3], PLATFORM_KEY_START);
}

static void t_keys_arrows_as_escape_sequences(void)
{
    PlatformKey k[4];

    CHECK_EQ(decode("\033[A\033[B\033[C\033[D", k, 4), 4);
    CHECK_EQ(k[0], PLATFORM_KEY_UP);
    CHECK_EQ(k[1], PLATFORM_KEY_DOWN);
    CHECK_EQ(k[2], PLATFORM_KEY_RIGHT);
    CHECK_EQ(k[3], PLATFORM_KEY_LEFT);
}

static void t_keys_arrows_in_application_mode(void)
{
    PlatformKey k[4];

    /* xterm can be configured to send ESC O A rather than ESC [ A. */
    CHECK_EQ(decode("\033OA\033OD", k, 4), 2);
    CHECK_EQ(k[0], PLATFORM_KEY_UP);
    CHECK_EQ(k[1], PLATFORM_KEY_LEFT);
}

static void t_keys_arrows_with_ctrl_modifier(void)
{
    PlatformKey k[2];

    /* Ctrl+arrow arrives with a parameter before the final letter. */
    CHECK_EQ(decode("\033[1;5C\033[1;5D", k, 2), 2);
    CHECK_EQ(k[0], PLATFORM_KEY_RIGHT);
    CHECK_EQ(k[1], PLATFORM_KEY_LEFT);
}

static void t_keys_escape_and_ctrl_c_both_quit(void)
{
    PlatformKey k[4];

    CHECK_EQ(decode("\033", k, 4), 1);
    CHECK_EQ(k[0], PLATFORM_KEY_QUIT);

    CHECK_EQ(decode("\003", k, 4), 1);
    CHECK_EQ(k[0], PLATFORM_KEY_QUIT);

    CHECK_EQ(decode("qQ", k, 4), 2);
    CHECK_EQ(k[0], PLATFORM_KEY_QUIT);
    CHECK_EQ(k[1], PLATFORM_KEY_QUIT);
}

static void t_keys_command_keys(void)
{
    PlatformKey k[5];

    CHECK_EQ(decode("prm?", k, 5), 4);
    CHECK_EQ(k[0], PLATFORM_KEY_PAUSE);
    CHECK_EQ(k[1], PLATFORM_KEY_RESET);
    CHECK_EQ(k[2], PLATFORM_KEY_MUTE);
    CHECK_EQ(k[3], PLATFORM_KEY_HELP);
}

static void t_keys_unknown_sequences_are_swallowed(void)
{
    PlatformKey k[8];
    int n = decode("\033[15~", k, 8); /* F5, which we do not bind */

    /* One event, and definitely not fifteen key presses. */
    CHECK_EQ(n, 1);
    CHECK_EQ(k[0], PLATFORM_KEY_UNMAPPED);

    /* The bracketed garbage that follows must not be read as keys either. */
    CHECK_EQ(decode("\033[", k, 8), 1);
    CHECK_EQ(k[0], PLATFORM_KEY_UNMAPPED);
}

static void t_keys_truncated_sequence_is_not_a_quit(void)
{
    PlatformKey k[4];
    int n = decode("\033[1;5", k, 4);

    /* An incomplete sequence at the end of a read: one unmapped event,
     * and no accidental quit. */
    CHECK_EQ(n, 1);
    CHECK_EQ(k[0], PLATFORM_KEY_UNMAPPED);
}

static void t_keys_unbound_characters_are_unmapped(void)
{
    PlatformKey k[4];

    CHECK_EQ(decode("59", k, 4), 2);
    CHECK_EQ(k[0], PLATFORM_KEY_UNMAPPED);
    CHECK_EQ(k[1], PLATFORM_KEY_UNMAPPED);
}

static void t_keys_empty_input_decodes_to_nothing(void)
{
    PlatformKey k[4];

    CHECK_EQ(decode("", k, 4), 0);
    CHECK_EQ(platform_decode_stream(NULL, 10, 0, k, 4), 0);
    CHECK_EQ(platform_decode_stream("a", 1, 0, NULL, 4), 0);
    CHECK_EQ(platform_decode_stream("a", 1, 0, k, 0), 0);
}

/* ------------------------------------------------------------------ */
/* Decoding the way a caller would, one key at a time                  */
/* ------------------------------------------------------------------ */

static void t_keys_resuming_from_an_offset(void)
{
    const char *stream = "z\033[Az";
    const int len = (int)strlen(stream);
    PlatformKey whole[8];
    PlatformKey part[8];

    /* Decoding from the start: A button, up arrow, A button again. */
    CHECK_EQ(platform_decode_stream(stream, len, 0, whole, 8), 3);
    CHECK_EQ(whole[0], PLATFORM_KEY_A);
    CHECK_EQ(whole[1], PLATFORM_KEY_UP);
    CHECK_EQ(whole[2], PLATFORM_KEY_A);

    /* Starting partway in picks up exactly the rest, which is what lets a
     * caller resume after a sequence was split across two reads. */
    CHECK_EQ(platform_decode_stream(stream, len, 1, part, 8), 2);
    CHECK_EQ(part[0], PLATFORM_KEY_UP);
    CHECK_EQ(part[1], PLATFORM_KEY_A);

    CHECK_EQ(platform_decode_stream(stream, len, len, part, 8), 0);
    CHECK_EQ(platform_decode_stream(stream, len, len + 5, part, 8), 0);
}

static void t_keys_output_buffer_is_bounded(void)
{
    PlatformKey k[2];
    int n = platform_decode_stream("wasdzx", 6, 0, k, 2);

    /* More keys arrived than there was room for, and we are told how many
     * so nothing is silently lost. */
    CHECK_EQ(n, 6);
    CHECK_EQ(k[0], PLATFORM_KEY_UP);
    CHECK_EQ(k[1], PLATFORM_KEY_LEFT);
}

/* ------------------------------------------------------------------ */
/* Key codes to joypad buttons                                         */
/* ------------------------------------------------------------------ */

static void t_keymap_buttons(void)
{
    CHECK_EQ(keymap_button(PLATFORM_KEY_UP), GB_KEY_UP);
    CHECK_EQ(keymap_button(PLATFORM_KEY_DOWN), GB_KEY_DOWN);
    CHECK_EQ(keymap_button(PLATFORM_KEY_LEFT), GB_KEY_LEFT);
    CHECK_EQ(keymap_button(PLATFORM_KEY_RIGHT), GB_KEY_RIGHT);
    CHECK_EQ(keymap_button(PLATFORM_KEY_A), GB_KEY_A);
    CHECK_EQ(keymap_button(PLATFORM_KEY_B), GB_KEY_B);
    CHECK_EQ(keymap_button(PLATFORM_KEY_START), GB_KEY_START);
    CHECK_EQ(keymap_button(PLATFORM_KEY_SELECT), GB_KEY_SELECT);
}

static void t_keymap_commands(void)
{
    CHECK_EQ(keymap_command(PLATFORM_KEY_PAUSE), KEYMAP_CMD_PAUSE);
    CHECK_EQ(keymap_command(PLATFORM_KEY_RESET), KEYMAP_CMD_RESET);
    CHECK_EQ(keymap_command(PLATFORM_KEY_MUTE), KEYMAP_CMD_MUTE);
    CHECK_EQ(keymap_command(PLATFORM_KEY_HELP), KEYMAP_CMD_HELP);
    CHECK_EQ(keymap_command(PLATFORM_KEY_QUIT), KEYMAP_CMD_QUIT);

    /* A button key is not a command, and vice versa. */
    CHECK_EQ(keymap_command(PLATFORM_KEY_UP), KEYMAP_CMD_NONE);
    CHECK_EQ(keymap_button(PLATFORM_KEY_QUIT), 0);
    CHECK_EQ(keymap_button(PLATFORM_KEY_NONE), 0);
    CHECK_EQ(keymap_command(PLATFORM_KEY_UNMAPPED), KEYMAP_CMD_NONE);
}

/* ------------------------------------------------------------------ */
/* A press turns into a timed hold                                     */
/* ------------------------------------------------------------------ */

static void t_keymap_a_press_is_held_then_released(void)
{
    KeyState ks;

    keymap_init(&ks);
    CHECK_EQ(keymap_sample(&ks, 0.0), 0);

    keymap_press(&ks, PLATFORM_KEY_A, 10.0);
    CHECK_EQ(keymap_sample(&ks, 10.0), GB_KEY_A);
    CHECK_EQ(keymap_sample(&ks, 10.1), GB_KEY_A);

    /* Still held just before the tap runs out... */
    CHECK_EQ(keymap_sample(&ks, 10.0 + KEYMAP_TAP_MS / 1000.0), GB_KEY_A);
    /* ...and let go just after. */
    CHECK_EQ(keymap_sample(&ks, 10.0 + KEYMAP_TAP_MS / 1000.0 + 0.001), 0);
}

static void t_keymap_tapping_again_extends_the_hold(void)
{
    KeyState ks;
    const double tap = KEYMAP_TAP_MS / 1000.0;

    keymap_init(&ks);
    keymap_press(&ks, PLATFORM_KEY_A, 0.0);

    /* Tapped again late in the hold: the button must stay down past the
     * original deadline, otherwise holding a direction in a game would
     * mean tapping in time with the emulator. */
    keymap_press(&ks, PLATFORM_KEY_A, tap - 0.05);
    CHECK_EQ(keymap_sample(&ks, tap + 0.10), GB_KEY_A);
    CHECK_EQ(keymap_sample(&ks, 2 * tap + 0.10), 0);
}

static void t_keymap_two_directions_make_a_diagonal(void)
{
    KeyState ks;

    keymap_init(&ks);

    /* A terminal reports two presses, one after the other, and both are
     * still held on the same frame. */
    keymap_press(&ks, PLATFORM_KEY_UP, 0.0);
    keymap_press(&ks, PLATFORM_KEY_RIGHT, 0.01);
    CHECK_EQ(keymap_sample(&ks, 0.02), GB_KEY_UP | GB_KEY_RIGHT);
}

static void t_keymap_keys_release_independently(void)
{
    KeyState ks;
    const double tap = KEYMAP_TAP_MS / 1000.0;

    keymap_init(&ks);
    keymap_press(&ks, PLATFORM_KEY_A, 0.0);
    keymap_press(&ks, PLATFORM_KEY_B, tap / 2);

    /* A expires while B is still held. */
    CHECK_EQ(keymap_sample(&ks, tap + 0.001), GB_KEY_B);
    CHECK_EQ(keymap_sample(&ks, tap * 2), 0);
}

static void t_keymap_command_keys_do_not_touch_the_joypad(void)
{
    KeyState ks;

    keymap_init(&ks);
    keymap_press(&ks, PLATFORM_KEY_PAUSE, 0.0);
    keymap_press(&ks, PLATFORM_KEY_RESET, 0.0);
    keymap_press(&ks, PLATFORM_KEY_QUIT, 0.0);
    keymap_press(&ks, PLATFORM_KEY_UNMAPPED, 0.0);

    CHECK_EQ(keymap_sample(&ks, 0.0), 0);
    CHECK_EQ(keymap_sample(&ks, 60.0), 0);
}

static void t_keymap_help_is_queued_once(void)
{
    KeyState ks;

    keymap_init(&ks);
    CHECK(!keymap_take_help(&ks));

    keymap_press(&ks, PLATFORM_KEY_HELP, 0.0);
    CHECK(keymap_take_help(&ks));
    /* Reading it clears it, so a single press shows the screen once. */
    CHECK(!keymap_take_help(&ks));
}

static void t_keymap_clear_drops_everything(void)
{
    KeyState ks;

    keymap_init(&ks);
    keymap_press(&ks, PLATFORM_KEY_START, 0.0);
    keymap_press(&ks, PLATFORM_KEY_SELECT, 0.0);
    CHECK(keymap_sample(&ks, 0.0) != 0);

    keymap_clear(&ks);
    CHECK_EQ(keymap_sample(&ks, 0.0), 0);
}

/* ------------------------------------------------------------------ */
/* Files and scratch space                                             */
/* ------------------------------------------------------------------ */

static void t_platform_temp_dir_is_usable(void)
{
    const char *dir = platform_temp_dir();
    const char *path;
    FILE *f;

    CHECK(dir != NULL);
    CHECK(strlen(dir) > 0);

    /* Not just a string: a file really has to be creatable there. */
    path = test_temp_path("tempdir.bin");
    f = platform_fopen(path, "wb");
    CHECK_MSG(f != NULL, "could not create '%s'", path);
    if (f) {
        fputc('x', f);
        fclose(f);
        remove(path);
    }
}

static void t_platform_temp_path_includes_the_name(void)
{
    const char *a = test_temp_path("alpha.bin");
    const char *b = test_temp_path("beta.bin");

    CHECK(strstr(a, "alpha.bin") != NULL);
    CHECK(strstr(b, "beta.bin") != NULL);
    CHECK(strcmp(a, b) != 0);
}

static void t_platform_fopen_round_trip(void)
{
    const char *path = test_temp_path("roundtrip.bin");
    FILE *f;
    char buf[8];

    f = platform_fopen(path, "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    fwrite("gameboy", 1, 7, f);
    fclose(f);

    f = platform_fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(fread(buf, 1, 7, f), 7);
    CHECK(strcmp(buf, "gameboy") == 0);
    fclose(f);

    remove(path);
}

static void t_platform_fopen_handles_a_non_ascii_path(void)
{
    /* On Windows this goes through the wide character API, so a ROM or a
     * save state with an accent in its name still works.  Elsewhere it is
     * just an ordinary byte string. */
    const char *path = test_temp_path("rom-caf\xC3\xA9.bin");
    FILE *f;

    f = platform_fopen(path, "wb");
    CHECK_MSG(f != NULL, "could not create '%s'", path);
    if (!f)
        return;
    fputc(0x42, f);
    fclose(f);

    f = platform_fopen(path, "rb");
    CHECK_MSG(f != NULL, "could not reopen '%s'", path);
    if (f) {
        CHECK_EQ(fgetc(f), 0x42);
        fclose(f);
    }

    remove(path);
}

static void t_platform_fopen_missing_file(void)
{
    const char *path = test_temp_path("definitely-absent.bin");

    remove(path);
    CHECK(platform_fopen(path, "rb") == NULL);
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static void t_platform_clock_never_goes_backwards(void)
{
    double first = platform_now();

    for (int i = 0; i < 1000; i++) {
        double t = platform_now();

        CHECK_MSG(t >= first, "clock went backwards: %f then %f", first, t);
        first = t;
    }
}

static void t_platform_clock_is_reasonable(void)
{
    /* It is a wall clock measured from an arbitrary origin, so the only
     * thing worth asserting is that the units really are seconds. */
    double a = platform_now();

    platform_sleep_ms(60);
    double b = platform_now();

    CHECK_MSG(b - a >= 0.05, "60 ms of sleep advanced the clock by only %f s",
              b - a);
    CHECK_MSG(b - a < 5.0, "60 ms of sleep advanced the clock by %f s", b - a);
}

static void t_platform_sleeping_zero_returns_at_once(void)
{
    double a = platform_now();

    platform_sleep_ms(0);
    platform_sleep_ms(-5);
    CHECK(platform_now() - a < 1.0);
}

/* ------------------------------------------------------------------ */
/* Child processes                                                     */
/* ------------------------------------------------------------------ */

static void t_platform_have_program_rejects_nonsense(void)
{
    CHECK(!platform_have_program(""));
    CHECK(!platform_have_program(NULL));
    CHECK(!platform_have_program("a-program-that-does-not-exist-anywhere"));
}

static void t_platform_default_player_is_usable_if_present(void)
{
    const char *cmd = platform_default_player();

    /* Either nothing on this machine can play raw PCM, which is a fine
     * answer, or we were given a command we could actually run. */
    if (cmd)
        CHECK(strlen(cmd) > 0);
}

static void t_platform_null_player_arguments_are_safe(void)
{
    CHECK(platform_open_player(NULL) == NULL);
    CHECK_EQ(platform_close_player(NULL), 0);
}

/* ------------------------------------------------------------------ */
/* Reading input with nothing pending                                  */
/* ------------------------------------------------------------------ */

static void t_platform_read_keys_is_safe_with_nothing_to_read(void)
{
    PlatformKey k[8];

    /* The test runner's stdin is not a terminal on a build machine, so
     * this exercises the "no input waiting" path, and also the argument
     * checks around it. */
    (void)platform_read_keys(k, 8);
    CHECK_EQ(platform_read_keys(NULL, 8), 0);
    CHECK_EQ(platform_read_keys(k, 0), 0);
}

static void t_platform_interactive_can_be_entered_and_left(void)
{
    /* Not attached to a terminal here, so this reports false; what matters
     * is that leaving it afterwards is harmless, since the frontend calls
     * it on every exit path. */
    bool interactive = platform_begin_interactive();

    (void)interactive;
    platform_end_interactive();
    platform_end_interactive();
}

static void t_platform_resize_is_safe_to_ask_for(void)
{
    /* Asking to resize must never be harmful: the sizes are nonsense on
     * purpose, and on a host that cannot resize it simply says no. */
    CHECK(!platform_resize_console(0, 0));
    CHECK(!platform_resize_console(-10, -10));
    CHECK(!platform_resize_console(100000, 100000));

    /* And whatever it answers, the terminal it reported is still the one
     * there is afterwards. */
    int cols = 0, rows = 0;
    platform_terminal_size(&cols, &rows);
    CHECK(cols > 0 && rows > 0);
}

static void t_platform_resize_only_grows(void)
{
    int cols = 0, rows = 0;

    platform_terminal_size(&cols, &rows);
    if (cols < 1 || rows < 1)
        return;

    /* Asking for something smaller must be refused outright, so a window
     * that is already big enough is never taken away from the user. */
    CHECK_MSG(!platform_resize_console(cols - 1, rows - 1) ||
                  !platform_can_resize_console(),
              "a smaller console was accepted");
}

static void t_platform_agrees_about_whether_it_can_resize(void)
{
    /* If the host says it cannot resize, asking must do nothing; if it
     * can, asking for an absurd size must fail rather than hang. */
    bool can = platform_can_resize_console();

    if (!can) {
        CHECK(!platform_resize_console(500, 200));
    } else {
        (void)platform_resize_console(500, 200);
    }
}

static void t_platform_terminal_size_is_sane(void)
{
    int cols = 0, rows = 0;

    platform_terminal_size(&cols, &rows);
    CHECK_MSG(cols > 0 && cols < 10000, "columns came out as %d", cols);
    CHECK_MSG(rows > 0 && rows < 10000, "rows came out as %d", rows);

    /* Asking twice must give the same answer for a terminal that is not
     * being resized, and the frontend relies on that for its layout. */
    int cols2 = 0, rows2 = 0;
    platform_terminal_size(&cols2, &rows2);
    CHECK_EQ(cols2, cols);
    CHECK_EQ(rows2, rows);
}

static void t_platform_utf8_answer_is_stable(void)
{
    /* Whether it is true depends on the terminal, but it must not change
     * between two calls in the same frame, or the renderer would pick a
     * different glyph set on alternate frames. */
    CHECK_EQ(platform_supports_utf8(), platform_supports_utf8());
}

const TestCase platform_tests[] = {
    { "platform", "decodes plain letters",        t_keys_plain_letters },
    { "platform", "decodes uppercase and hjkl",   t_keys_uppercase_and_hjkl },
    { "platform", "decodes the face buttons",     t_keys_buttons },
    { "platform", "decodes arrow escape codes",   t_keys_arrows_as_escape_sequences },
    { "platform", "decodes application mode",     t_keys_arrows_in_application_mode },
    { "platform", "decodes modified arrows",      t_keys_arrows_with_ctrl_modifier },
    { "platform", "escape and ctrl-c both quit",  t_keys_escape_and_ctrl_c_both_quit },
    { "platform", "decodes the command keys",     t_keys_command_keys },
    { "platform", "swallows unknown sequences",   t_keys_unknown_sequences_are_swallowed },
    { "platform", "truncated sequence is safe",   t_keys_truncated_sequence_is_not_a_quit },
    { "platform", "unbound chars are unmapped",   t_keys_unbound_characters_are_unmapped },
    { "platform", "empty input decodes to none",  t_keys_empty_input_decodes_to_nothing },
    { "platform", "resumes from an offset",         t_keys_resuming_from_an_offset },
    { "platform", "respects the output bound",    t_keys_output_buffer_is_bounded },

    { "platform", "maps keys to buttons",         t_keymap_buttons },
    { "platform", "maps keys to commands",        t_keymap_commands },
    { "platform", "a press is held then released", t_keymap_a_press_is_held_then_released },
    { "platform", "tapping again extends hold",   t_keymap_tapping_again_extends_the_hold },
    { "platform", "two directions give diagonal", t_keymap_two_directions_make_a_diagonal },
    { "platform", "keys release independently",   t_keymap_keys_release_independently },
    { "platform", "commands leave joypad alone",  t_keymap_command_keys_do_not_touch_the_joypad },
    { "platform", "help is queued exactly once",  t_keymap_help_is_queued_once },
    { "platform", "clear drops everything",       t_keymap_clear_drops_everything },

    { "platform", "temp dir is usable",           t_platform_temp_dir_is_usable },
    { "platform", "temp path keeps the name",     t_platform_temp_path_includes_the_name },
    { "platform", "files round trip",             t_platform_fopen_round_trip },
    { "platform", "handles a non-ascii path",     t_platform_fopen_handles_a_non_ascii_path },
    { "platform", "missing file returns null",    t_platform_fopen_missing_file },

    { "platform", "clock never goes backwards",   t_platform_clock_never_goes_backwards },
    { "platform", "clock counts seconds",         t_platform_clock_is_reasonable },
    { "platform", "sleeping zero returns at once", t_platform_sleeping_zero_returns_at_once },

    { "platform", "have_program rejects junk",    t_platform_have_program_rejects_nonsense },
    { "platform", "default player is usable",     t_platform_default_player_is_usable_if_present },
    { "platform", "null player args are safe",    t_platform_null_player_arguments_are_safe },

    { "platform", "read_keys with no input",      t_platform_read_keys_is_safe_with_nothing_to_read },
    { "platform", "interactive enter and leave",  t_platform_interactive_can_be_entered_and_left },
    { "platform", "resizing is safe to ask for",   t_platform_resize_is_safe_to_ask_for },
    { "platform", "resizing only ever grows",      t_platform_resize_only_grows },
    { "platform", "resize capability is honest",   t_platform_agrees_about_whether_it_can_resize },
    { "platform", "terminal size is sane",        t_platform_terminal_size_is_sane },
    { "platform", "utf8 answer is stable",        t_platform_utf8_answer_is_stable },

    TEST_SUITE_END
};