#include "boot_unlock_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    BootUnlockConfig config;
    bootUnlockConfigDefaults(&config);
    assert(config.mode == BootUnlockModeArmed);
    assert(config.firstDelayMs == BOOT_UNLOCK_DEFAULT_FIRST_DELAY_MS);
    assert(config.wakeDelayMs == BOOT_UNLOCK_DEFAULT_WAKE_DELAY_MS);
    assert(config.wakeAttempts == BOOT_UNLOCK_DEFAULT_WAKE_ATTEMPTS);
    assert(config.verifyDelayMs == BOOT_UNLOCK_DEFAULT_VERIFY_DELAY_MS);
    assert(config.pressGapMs == BOOT_UNLOCK_DEFAULT_PRESS_GAP_MS);
    assert(config.burstPresses == BOOT_UNLOCK_DEFAULT_BURST_PRESSES);
    assert(config.maxRounds == BOOT_UNLOCK_DEFAULT_MAX_ROUNDS);
    assert(config.deadlineMs == BOOT_UNLOCK_DEFAULT_DEADLINE_MS);

    /* The whole section, with the whitespace and comments a hand-edited file
     * carries. */
    const char* valid =
        "; boot unlock tuning\n[other]\nmode=off\n[boot-unlock]\n"
        " mode = everyBoot   # press on every boot\n"
        "firstDelayMs=1500\nwakeDelayMs = 750\nwakeAttempts=2\nverifyDelayMs=900\n"
        "pressGapMs=300\nburstPresses=4\nmaxRounds=5\ndeadlineMs=20000\n";
    assert(bootUnlockConfigParseText(valid, &config) == BootUnlockConfigOk);
    assert(config.mode == BootUnlockModeEveryBoot);
    assert(config.firstDelayMs == 1500 && config.wakeDelayMs == 750);
    assert(config.wakeAttempts == 2 && config.verifyDelayMs == 900);
    assert(config.pressGapMs == 300 && config.burstPresses == 4);
    assert(config.maxRounds == 5 && config.deadlineMs == 20000);

    /* Leaving values out keeps the defaults for them. */
    assert(bootUnlockConfigParseText("[boot-unlock]\nmode=armed\n", &config)
        == BootUnlockConfigOk);
    assert(config.mode == BootUnlockModeArmed);
    assert(config.firstDelayMs == BOOT_UNLOCK_DEFAULT_FIRST_DELAY_MS);
    assert(config.burstPresses == BOOT_UNLOCK_DEFAULT_BURST_PRESSES);

    /* A key this build does not know is ignored, so a newer file still works. */
    assert(bootUnlockConfigParseText("[boot-unlock]\nbutton=Y\nmode=off\n", &config)
        == BootUnlockConfigOk);
    assert(config.mode == BootUnlockModeOff);

    /* Values that would make the worker press forever are rejected outright. */
    assert(bootUnlockConfigParseText("[boot-unlock]\nmode=sometimes\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\nmaxRounds=0\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\nburstPresses=0\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\nmaxRounds=9999\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\nwakeDelayMs=100000\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\nverifyDelayMs=100000\n", &config)
        == BootUnlockConfigInvalidValue);
    /* The burst has to stay inside the lock screen's ~1 s window. */
    assert(bootUnlockConfigParseText("[boot-unlock]\npressGapMs=1000\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\npressGapMs=500\n", &config)
        == BootUnlockConfigOk);
    assert(bootUnlockConfigParseText("[boot-unlock]\nfirstDelayMs=abc\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock\nmode=off\n", &config)
        == BootUnlockConfigInvalidValue);
    assert(bootUnlockConfigParseText("[boot-unlock]\nno-equals\n", &config)
        == BootUnlockConfigInvalidValue);

    /* A broken value must not leave a half-applied schedule behind. */
    assert(bootUnlockConfigParseText("[boot-unlock]\nmaxRounds=7\nmode=broken\n", &config)
        == BootUnlockConfigInvalidValue);

    assert(!strcmp(bootUnlockModeName(BootUnlockModeArmed), "armed"));
    assert(!strcmp(bootUnlockModeName(BootUnlockModeEveryBoot), "everyBoot"));
    assert(!strcmp(bootUnlockModeName(BootUnlockModeOff), "off"));

    BootUnlockMode mode = BootUnlockModeArmed;
    assert(bootUnlockModeFromName("off", &mode) && mode == BootUnlockModeOff);
    assert(bootUnlockModeFromName("everyBoot", &mode) && mode == BootUnlockModeEveryBoot);
    assert(!bootUnlockModeFromName("yes", &mode));

    puts("boot_unlock_config_test: ok");
    return 0;
}
