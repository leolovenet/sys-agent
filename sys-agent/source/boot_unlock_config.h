#ifndef SYSAGENT_BOOT_UNLOCK_CONFIG_H
#define SYSAGENT_BOOT_UNLOCK_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Boot unlock: press A with the virtual controller as soon as the console is up,
 * so the start screen Horizon shows after a reboot ("A 开始") does not time out
 * into sleep and take the network stack - and with it every sys-agent session -
 * down with it.
 *
 * The schedule lives in an INI file on the SD card so it can be tuned without
 * rebuilding the sysmodule; the arm file is what the reboot path plants to ask
 * for one unlock on the next boot (its presence is the whole request). */
#define BOOT_UNLOCK_CONFIG_PATH "sdmc:/config/sys-agent/boot-unlock.ini"
#define BOOT_UNLOCK_ARM_PATH "sdmc:/config/sys-agent/boot-unlock.armed"

/* Measured on the development console: Horizon shows its lock screen around 20 s
 * after boot, darkens the display about a dozen seconds later, and leaves the
 * console reachable - but locked - for a couple of minutes before it sleeps. The
 * unlock therefore does not need a precise moment: it looks at the screen, acts on
 * what it sees, and gives up on a deadline.
 *
 * HOME is the button to use. It is inert on both lock screens and on the HOME menu,
 * and it is what brings a darkened display back. The plain lock screen opens with
 * one HOME; with "Screen Lock" enabled the console first shows the same screen and
 * the press leads to a second lock screen that wants the same button three times
 * within about a second - so that one is answered with a tight burst and no capture
 * in between. */
#define BOOT_UNLOCK_DEFAULT_FIRST_DELAY_MS 2000
#define BOOT_UNLOCK_DEFAULT_WAKE_ATTEMPTS 4
#define BOOT_UNLOCK_DEFAULT_WAKE_DELAY_MS 600
#define BOOT_UNLOCK_DEFAULT_VERIFY_DELAY_MS 1500
#define BOOT_UNLOCK_DEFAULT_PRESS_GAP_MS 250
#define BOOT_UNLOCK_DEFAULT_BURST_PRESSES 3
#define BOOT_UNLOCK_DEFAULT_MAX_ROUNDS 8
#define BOOT_UNLOCK_DEFAULT_DEADLINE_MS 90000
#define BOOT_UNLOCK_MAX_FIRST_DELAY_MS 600000
#define BOOT_UNLOCK_MAX_WAKE_ATTEMPTS 10
#define BOOT_UNLOCK_MAX_WAKE_DELAY_MS 60000
#define BOOT_UNLOCK_MAX_VERIFY_DELAY_MS 60000
#define BOOT_UNLOCK_MAX_PRESS_GAP_MS 1000
#define BOOT_UNLOCK_MAX_BURST_PRESSES 10
#define BOOT_UNLOCK_MAX_ROUNDS 32
#define BOOT_UNLOCK_MAX_DEADLINE_MS 600000

/* When the boot unlock runs. `armed` is the default, so a build that is not
 * configured on the SD card never presses anything on its own. */
typedef enum {
    BootUnlockModeOff = 0,
    BootUnlockModeArmed = 1,
    BootUnlockModeEveryBoot = 2
} BootUnlockMode;

typedef struct {
    BootUnlockMode mode;
    uint32_t firstDelayMs;
    uint32_t wakeAttempts;  /* HOME presses allowed while the display stays dark */
    uint32_t wakeDelayMs;   /* pause after a wake press */
    uint32_t verifyDelayMs; /* pause after a press before looking again */
    uint32_t pressGapMs;    /* gap inside the three-press burst (must stay < 1 s) */
    uint32_t burstPresses;  /* presses for the "press the same button 3 times" screen */
    uint32_t maxRounds;     /* capture/act rounds before giving up */
    uint32_t deadlineMs;    /* total budget, measured from the worker's start */
} BootUnlockConfig;

typedef enum {
    BootUnlockConfigOk = 0,
    BootUnlockConfigNotFound,
    BootUnlockConfigInvalidValue,
    BootUnlockConfigIoError
} BootUnlockConfigResult;

void bootUnlockConfigDefaults(BootUnlockConfig* config);
/* Parses the [boot-unlock] section. Unknown keys are ignored (so the file can
 * carry comments for future options), bad values are rejected. */
BootUnlockConfigResult bootUnlockConfigParseText(const char* text, BootUnlockConfig* config);
/* Reads `path`; a missing file reports NotFound (the caller then keeps the
 * defaults), an unreadable or unparsable one reports InvalidValue/IoError. */
BootUnlockConfigResult bootUnlockConfigLoad(const char* path, BootUnlockConfig* config);
const char* bootUnlockConfigResultName(BootUnlockConfigResult result);

const char* bootUnlockModeName(BootUnlockMode mode);
bool bootUnlockModeFromName(const char* name, BootUnlockMode* mode);

#endif
