/* sm64ex's configuration globals with their upstream defaults (configfile.c), minus the INI
 * parser: the Zune build has no config file or options menu yet. C89 for VC9. */
#include "pc/configfile.h"
#include "pc/controller/controller_api.h"
#include "pc/gfx/gfx_window_manager_api.h"

ConfigWindow configWindow = {WAPI_WIN_CENTERPOS, WAPI_WIN_CENTERPOS, 480, 272, 1, 0, 1, 0, 0};
unsigned int configFiltering = 1;            /* linear; the precompiled shaders assume 1 */
unsigned int configMasterVolume = MAX_VOLUME;
unsigned int configMusicVolume = MAX_VOLUME;
unsigned int configSfxVolume = MAX_VOLUME;
unsigned int configEnvVolume = MAX_VOLUME;

/* Keyboard/gamepad bindings are unused (touch controls); kept for linkage. */
unsigned int configKeyA[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyB[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyStart[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyL[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyR[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyZ[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyCUp[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyCDown[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyCLeft[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyCRight[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyStickUp[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyStickDown[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyStickLeft[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configKeyStickRight[MAX_BINDS] = {VK_INVALID, VK_INVALID, VK_INVALID};
unsigned int configStickDeadzone = 16;
unsigned int configRumbleStrength = 0;
bool configSkipIntro = 0;
bool configHUD = 1;
