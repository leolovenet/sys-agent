#pragma once

#include <stdbool.h>

/* Called once from main() after the services are up. `servicesReady` says whether
 * the SD card got mounted: the schedule and the arm file are files on it, so
 * without that this boot cannot unlock (the command below still reports why
 * instead of pretending nothing happened). */
void bootUnlockInit(bool servicesReady);

/* `bootUnlock` - the schedule this boot was configured with and what it did. */
void bootUnlockStatusCommand(void);
/* `bootUnlockArm <enabled|disabled>` - park or drop the one-shot arm file that
 * asks for an unlock on the next boot. */
void bootUnlockArmCommand(bool arm);
/* Writes the one-shot request file. The reboot commands call this so that a
 * reboot unlocks by default; callers that do not want that pass noUnlock. */
bool bootUnlockRequest(void);
/* `bootUnlockRun` - run the schedule right now instead of waiting for a boot.
 * Careful: on the HOME menu a stray A starts the selected game. */
void bootUnlockRunCommand(void);
