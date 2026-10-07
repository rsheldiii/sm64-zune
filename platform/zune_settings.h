#ifndef ZUNE_SETTINGS_H
#define ZUNE_SETTINGS_H
// Build-time settings of the Zune HD port: this file is the list of them, with their
// defaults. Do not edit it to configure your own build. Instead put NAME=VALUE lines in
// settings.local in the project root (git ignores it) for what you always want, or pass
// --define NAME=VALUE to ./sm64zune build for one build. A value is a number or one of the
// words listed for the setting. `./sm64zune settings` prints every setting with the value
// the next build would use.
//
// tools/settings.py reads this file. To add a setting, follow the pattern: a comment that
// describes it, lines "N = word" naming its values or one line "LOW..HIGH" for a range, then
// the #ifndef/#define pair with the default. Then use the macro in the code.
#include "zune_build.h"   // the values a build chose; the defaults below cover the rest

// What happens when a tick of the game takes longer than its 1/30 s (zune_pacing.h).
//   0 = never      no frame is skipped; slow areas run in slow motion, as on the N64
//   1 = sustained  drawing is skipped only where the game has been slow for about half a
//                  second or more; a short burst of lag slows the game for a moment instead
//   2 = always     drawing is skipped whenever the game is behind, so it keeps real time
#ifndef ZUNE_FRAME_SKIP
#define ZUNE_FRAME_SKIP 1
#endif

// How the touch controls are drawn. Touches work the same in every style.
//   0 = hidden     nothing is drawn
//   1 = outline    the round outline of each button; a press fills it faintly
//   2 = filled     tinted discs with an outline and the button's letter or arrow
#ifndef ZUNE_TOUCH_BUTTONS
#define ZUNE_TOUCH_BUTTONS 2
#endif

// Opacity of the touch controls, in percent of their full strength.
//   10..100
#ifndef ZUNE_TOUCH_OPACITY
#define ZUNE_TOUCH_OPACITY 80
#endif

// A diary of the run for finding faults: start-up steps, timing every five seconds, and the
// details of a crash. It is kept in \Flash2\SM64\log.txt on the Zune, and nothing can read
// that over USB, so on exit (hold three fingers on the screen) the game sends it over Wi-Fi
// to ZUNE_LOG_HOST, where `./sm64zune logs` is waiting for it.
//   0 = off        nothing is written or sent; leaving the game is immediate
//   1 = on
#ifndef ZUNE_LOG
#define ZUNE_LOG 0
#endif

// Where the log is sent: the computer that runs `./sm64zune logs`. Left empty, a build with
// ZUNE_LOG=on uses the address of the computer it is built on.
//   IPv4 address
#ifndef ZUNE_LOG_HOST
#define ZUNE_LOG_HOST ""
#endif

// For working on the port: a sampling profiler records where the game spends its time and
// sends that with the log (read it with tools/profile.py). It costs some speed. Needs
// ZUNE_LOG=on.
//   0 = off
//   1 = on
#ifndef ZUNE_PROFILE
#define ZUNE_PROFILE 0
#endif

// tools/settings.py checks values before a build; this catches a /D that bypassed it.
#if ZUNE_FRAME_SKIP < 0 || ZUNE_FRAME_SKIP > 2 || ZUNE_TOUCH_BUTTONS < 0 || ZUNE_TOUCH_BUTTONS > 2 \
    || ZUNE_TOUCH_OPACITY < 10 || ZUNE_TOUCH_OPACITY > 100 || ZUNE_LOG < 0 || ZUNE_LOG > 1 \
    || ZUNE_PROFILE < 0 || ZUNE_PROFILE > 1 || (ZUNE_PROFILE && !ZUNE_LOG)
#error A build setting is out of range; see zune_settings.h.
#endif

#endif
