#include "boot_unlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "boot_unlock_config.h"
#include "commands.h"
#include "jpeg_dc.h"
#include "screen_classify.h"
#include "search_store.h"
#include "util.h"

/* The worker owns its own thread so a slow capture (they take seconds while the
 * display is dark) never delays command handling. */
#define BOOT_UNLOCK_THREAD_SIZE 0x8000
#define BOOT_UNLOCK_SCREEN_BUFFER_SIZE 0x7D000   /* capssc's JPEG buffer */
#define BOOT_UNLOCK_VERDICTS 12
#define BOOT_UNLOCK_LOG_PATH "sdmc:/config/sys-agent/boot-unlock.log"

typedef enum {
    BootUnlockPhaseIdle = 0,
    BootUnlockPhaseRunning,
    BootUnlockPhaseDone
} BootUnlockPhase;

typedef struct {
    uint32_t ms;              /* milliseconds since boot */
    ScreenClass verdict;
    ScreenMetrics metrics;
} BootUnlockVerdict;

typedef struct {
    bool servicesReady;
    const char* reason;
    const char* source;
    BootUnlockConfig config;
    bool armedAtStart;
    bool armCleared;
    volatile BootUnlockPhase phase;
    u64 startTick;
    u64 endTick;
    u32 rounds;
    u32 wakePresses;          /* HOME presses sent just to light the display */
    u32 homePresses;          /* HOME presses sent to unlock */
    u32 homeSent;             /* of those, the ones HOS accepted */
    u32 applicationRuns;
    BootUnlockVerdict verdicts[BOOT_UNLOCK_VERDICTS];
    u32 verdictCount;
} BootUnlock;

static BootUnlock bootUnlock = {
    .reason = "init",
    .source = "none",
    .phase = BootUnlockPhaseIdle,
};
static Thread bootUnlockThread;
static Mutex bootUnlockMutex;
static bool bootUnlockMutexReady = false;
static bool bootUnlockThreadCreated = false;
static bool bootUnlockRequested = false;

static bool bootUnlockStartWorker(void);

static u64 bootUnlockTicksFromMs(u32 ms)
{
    return (u64)ms * armGetSystemTickFreq() / 1000;
}

static u64 bootUnlockMsAt(u64 tick)
{
    if (tick == 0)
        return 0;
    return tick * 1000 / armGetSystemTickFreq();
}

/* Sleeps in short steps: the delays here are hundreds of milliseconds and the
 * worker has to notice its deadline promptly. */
static void bootUnlockSleepMs(u32 ms)
{
    while (ms > 0) {
        u32 step = ms > 100 ? 100 : ms;
        svcSleepThread((s64)step * 1000000LL);
        ms -= step;
    }
}

static bool bootUnlockArmFileExists(void)
{
    FILE* file = fopen(BOOT_UNLOCK_ARM_PATH, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

static void bootUnlockRecord(ScreenClass verdict, const ScreenMetrics* metrics, u32 ms)
{
    mutexLock(&bootUnlockMutex);
    if (bootUnlock.verdictCount < BOOT_UNLOCK_VERDICTS) {
        BootUnlockVerdict* entry = &bootUnlock.verdicts[bootUnlock.verdictCount++];
        entry->ms = ms;
        entry->verdict = verdict;
        if (metrics != NULL)
            entry->metrics = *metrics;
        else
            memset(&entry->metrics, 0, sizeof(entry->metrics));
    }
    mutexUnlock(&bootUnlockMutex);
}

/* One HOME click. Returns whether HOS accepted the virtual controller's state
 * write, i.e. whether there was a device for the press to land on. */
static bool bootUnlockPressHome(void)
{
    u64 before = controllerStateWriteSuccesses();
    click(HiddbgNpadButton_Home);
    bool landed = controllerStateWriteSuccesses() > before;
    mutexLock(&bootUnlockMutex);
    if (landed)
        bootUnlock.homeSent++;
    mutexUnlock(&bootUnlockMutex);
    return landed;
}

static ScreenClass bootUnlockLook(u8* buffer, JpegDcImage* image, ScreenMetrics* metrics,
                                  const char** detail)
{
    *detail = "capture-failed";
    u64 outSize = 0;
    Result rc = capsscCaptureForDebug(buffer, BOOT_UNLOCK_SCREEN_BUFFER_SIZE, &outSize);
    if (R_FAILED(rc) || outSize == 0) {
        /* The display is off (or capssc refused): there is no picture to judge, so
         * the only safe move is to try to light the screen up again. */
        memset(metrics, 0, sizeof(*metrics));
        return ScreenDark;
    }
    if (!jpegDcDecode(buffer, (size_t)outSize, image)) {
        *detail = "decode-failed";
        memset(metrics, 0, sizeof(*metrics));
        return ScreenUnknown;
    }
    ScreenClass verdict = screenClassify(image, metrics);
    *detail = screenClassName(verdict);
    return verdict;
}

/* Reads the schedule, checks the arm file and starts the worker. Called from
 * main() once the services are up; the worker is what looks at the screen and
 * presses the button. */
void bootUnlockInit(bool servicesReady)
{
    mutexInit(&bootUnlockMutex);
    bootUnlockMutexReady = true;

    bootUnlock.servicesReady = servicesReady;
    bootUnlockConfigDefaults(&bootUnlock.config);
    if (!servicesReady) {
        bootUnlock.reason = "no-sd-card";
        return;
    }

    BootUnlockConfig config;
    BootUnlockConfigResult result = bootUnlockConfigLoad(BOOT_UNLOCK_CONFIG_PATH, &config);
    if (result == BootUnlockConfigOk) {
        bootUnlock.config = config;
        bootUnlock.source = "file";
    }
    else if (result == BootUnlockConfigNotFound) {
        bootUnlock.source = "default";
    }
    else {
        /* A config file that cannot be used must not silently change what the
         * console does at boot: keep the defaults and say so in the report. */
        bootUnlock.source = "default";
        bootUnlock.reason = result == BootUnlockConfigIoError ? "config-io-error" : "config-invalid";
    }

    bootUnlock.armedAtStart = bootUnlockArmFileExists();
    bool wanted = false;
    switch (bootUnlock.config.mode) {
        case BootUnlockModeEveryBoot:
            wanted = true;
            bootUnlock.reason = "mode-every-boot";
            break;
        case BootUnlockModeArmed:
            wanted = bootUnlock.armedAtStart;
            bootUnlock.reason = wanted ? "armed" : "not-armed";
            break;
        case BootUnlockModeOff:
        default:
            bootUnlock.reason = "mode-off";
            break;
    }

    if (!wanted) {
        /* An arm file that asks for a mode this boot does not run is stale: drop it
         * so it cannot fire out of context later. */
        if (bootUnlock.armedAtStart && bootUnlock.config.mode == BootUnlockModeOff)
            bootUnlock.armCleared = remove(BOOT_UNLOCK_ARM_PATH) == 0;
        return;
    }

    bootUnlockRequested = true;
    if (!bootUnlockStartWorker())
        bootUnlock.reason = "thread-start-failed";
}

static bool bootUnlockApplicationRunning(void)
{
    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)))
        return false;
    return pid != 0;
}

static void bootUnlockWriteLog(void)
{
    searchStorePrepareDirectory("sdmc:/config/sys-agent");
    FILE* file = fopen(BOOT_UNLOCK_LOG_PATH, "a");
    if (file == NULL)
        return;
    fprintf(file, "boot unlock: reason=%s rounds=%u wake=%u home=%u sent=%u app=%u verdicts=",
        bootUnlock.reason, bootUnlock.rounds, bootUnlock.wakePresses, bootUnlock.homePresses,
        bootUnlock.homeSent, bootUnlock.applicationRuns);
    for (u32 i = 0; i < bootUnlock.verdictCount; i++)
        fprintf(file, "%s%s@%u(%.2f/%.1f/%.2f)", i ? "," : "",
            screenClassName(bootUnlock.verdicts[i].verdict), bootUnlock.verdicts[i].ms,
            bootUnlock.verdicts[i].metrics.rightFlat, bootUnlock.verdicts[i].metrics.rightSpread,
            bootUnlock.verdicts[i].metrics.leftFlat);
    fprintf(file, "\n");
    fclose(file);
}

static void bootUnlockWorker(void* arg)
{
    (void)arg;
    BootUnlockConfig config;
    mutexLock(&bootUnlockMutex);
    config = bootUnlock.config;
    bootUnlock.startTick = armGetSystemTick();
    bootUnlock.phase = BootUnlockPhaseRunning;
    mutexUnlock(&bootUnlockMutex);

    u8* buffer = malloc(BOOT_UNLOCK_SCREEN_BUFFER_SIZE);
    JpegDcImage* image = malloc(sizeof(JpegDcImage));
    if (buffer == NULL || image == NULL) {
        free(buffer);
        free(image);
        mutexLock(&bootUnlockMutex);
        bootUnlock.reason = "no-memory";
        bootUnlock.endTick = armGetSystemTick();
        bootUnlock.phase = BootUnlockPhaseDone;
        mutexUnlock(&bootUnlockMutex);
        return;
    }

    bootUnlockSleepMs(config.firstDelayMs);

    u64 deadlineTick = bootUnlock.startTick + bootUnlockTicksFromMs(config.deadlineMs);
    bool pressed = false;
    bool decided = false;   /* a break already picked the reason to report */
    ScreenClass lastVerdict = ScreenUnknown;
    for (u32 round = 0; round < config.maxRounds; round++) {
        if (armGetSystemTick() >= deadlineTick) {
            bootUnlock.reason = "deadline";
            decided = true;
            break;
        }
        if (bootUnlockApplicationRunning()) {
            /* A game is running: this boot is not waiting for an unlock, and the
             * unlock must not press anything into a game. */
            mutexLock(&bootUnlockMutex);
            bootUnlock.applicationRuns++;
            bootUnlock.reason = "application-running";
            mutexUnlock(&bootUnlockMutex);
            decided = true;
            break;
        }

        ScreenMetrics metrics;
        const char* detail = NULL;
        ScreenClass verdict = bootUnlockLook(buffer, image, &metrics, &detail);
        lastVerdict = verdict;
        bootUnlockRecord(verdict, &metrics, (u32)bootUnlockMsAt(armGetSystemTick()));

        if (verdict == ScreenOther) {
            /* Whatever is on screen is not a lock screen - either the console was
             * never locked, or it just opened. Nothing gets pressed either way. */
            bootUnlock.reason = pressed ? "unlocked" : "not-locked";
            decided = true;
            break;
        }
        if (verdict == ScreenUnknown) {
            bootUnlock.reason = detail ? detail : "unknown-screen";
            decided = true;
            break;
        }
        if (verdict == ScreenDark) {
            u32 wake = 0;
            mutexLock(&bootUnlockMutex);
            wake = bootUnlock.wakePresses;
            mutexUnlock(&bootUnlockMutex);
            if (wake >= config.wakeAttempts) {
                /* HOME can need a couple of tries, but past this point it is not
                 * going to light the display. */
                bootUnlock.reason = "display-stays-dark";
                decided = true;
                break;
            }
            bootUnlockPressHome();
            mutexLock(&bootUnlockMutex);
            bootUnlock.wakePresses++;
            mutexUnlock(&bootUnlockMutex);
            bootUnlockSleepMs(config.wakeDelayMs);
            continue;
        }
        if (verdict == ScreenLockOne) {
            bootUnlockPressHome();
            mutexLock(&bootUnlockMutex);
            bootUnlock.homePresses++;
            mutexUnlock(&bootUnlockMutex);
            pressed = true;
            bootUnlockSleepMs(config.verifyDelayMs);
            continue;
        }
        if (verdict == ScreenLockThree) {
            /* "Press the same button three times": the presses have to follow each
             * other inside about a second, so this is one burst with no capture in
             * between. HOME cannot start a game, which is what makes a burst safe. */
            for (u32 press = 0; press < config.burstPresses; press++) {
                if (press > 0)
                    bootUnlockSleepMs(config.pressGapMs);
                bootUnlockPressHome();
                mutexLock(&bootUnlockMutex);
                bootUnlock.homePresses++;
                mutexUnlock(&bootUnlockMutex);
            }
            pressed = true;
            bootUnlockSleepMs(config.verifyDelayMs);
            continue;
        }
    }
    if (!decided) {
        /* The rounds ran out with the screen still locked (or a burst in flight):
         * say so instead of claiming an unlock that did not happen. */
        bootUnlock.reason = lastVerdict == ScreenOther
            ? (pressed ? "unlocked" : "not-locked") : "rounds-exhausted";
    }

    u32 rounds = 0;
    mutexLock(&bootUnlockMutex);
    rounds = (u32)bootUnlock.verdictCount;
    mutexUnlock(&bootUnlockMutex);

    /* One shot: the boot that ran the unlock consumes the request. */
    bool armed = false;
    mutexLock(&bootUnlockMutex);
    armed = bootUnlock.armedAtStart;
    mutexUnlock(&bootUnlockMutex);
    bool cleared = false;
    if (armed)
        cleared = remove(BOOT_UNLOCK_ARM_PATH) == 0;

    mutexLock(&bootUnlockMutex);
    bootUnlock.rounds = rounds;
    if (cleared)
        bootUnlock.armCleared = true;
    bootUnlock.endTick = armGetSystemTick();
    bootUnlock.phase = BootUnlockPhaseDone;
    bootUnlockWriteLog();   /* reads the fields above, so it runs under the lock */
    mutexUnlock(&bootUnlockMutex);

    free(buffer);
    free(image);
}

static bool bootUnlockStartWorker(void)
{
    if (bootUnlockThreadCreated) {
        if (bootUnlock.phase == BootUnlockPhaseRunning)
            return false;
        /* The thread entry is done; the thread object cannot be restarted, so a
         * manual re-run rebuilds it. */
        threadClose(&bootUnlockThread);
        bootUnlockThreadCreated = false;
    }

    mutexLock(&bootUnlockMutex);
    bootUnlock.startTick = 0;
    bootUnlock.endTick = 0;
    bootUnlock.rounds = 0;
    bootUnlock.wakePresses = 0;
    bootUnlock.homePresses = 0;
    bootUnlock.homeSent = 0;
    bootUnlock.applicationRuns = 0;
    bootUnlock.verdictCount = 0;
    bootUnlock.phase = BootUnlockPhaseIdle;
    mutexUnlock(&bootUnlockMutex);

    Result rc = threadCreate(&bootUnlockThread, bootUnlockWorker, NULL, NULL,
        BOOT_UNLOCK_THREAD_SIZE, 0x2C, -2);
    if (R_FAILED(rc)) {
        logDiagnostic("threadCreate (bootUnlock)", rc);
        return false;
    }
    rc = threadStart(&bootUnlockThread);
    if (R_FAILED(rc)) {
        logDiagnostic("threadStart (bootUnlock)", rc);
        threadClose(&bootUnlockThread);
        return false;
    }
    bootUnlockThreadCreated = true;
    return true;
}

void bootUnlockStatusCommand(void)
{
    mutexLock(&bootUnlockMutex);
    BootUnlockConfig config = bootUnlock.config;
    const char* reason = bootUnlock.reason;
    const char* source = bootUnlock.source;
    bool armed = bootUnlock.armedAtStart;
    bool cleared = bootUnlock.armCleared;
    bool servicesReady = bootUnlock.servicesReady;
    bool requested = bootUnlockRequested;
    BootUnlockPhase phase = bootUnlock.phase;
    u64 startTick = bootUnlock.startTick;
    u64 endTick = bootUnlock.endTick;
    u32 rounds = bootUnlock.rounds;
    u32 wakePresses = bootUnlock.wakePresses;
    u32 homePresses = bootUnlock.homePresses;
    u32 homeSent = bootUnlock.homeSent;
    u32 applicationRuns = bootUnlock.applicationRuns;

    /* `screens` is the verdict timeline: what the unlock saw, and the metrics that
     * produced the verdict (right flat ratio / its colour spread / left flat). */
    char screens[BOOT_UNLOCK_VERDICTS * 40];
    size_t used = 0;
    ScreenClass lastClass = ScreenUnknown;
    ScreenMetrics lastMetrics;
    memset(&lastMetrics, 0, sizeof(lastMetrics));
    for (u32 i = 0; i < bootUnlock.verdictCount && i < BOOT_UNLOCK_VERDICTS; i++) {
        const BootUnlockVerdict* entry = &bootUnlock.verdicts[i];
        int written = snprintf(screens + used, sizeof(screens) - used, "%s%s@%u(%.2f/%.1f/%.2f)",
            i ? "," : "", screenClassName(entry->verdict), entry->ms,
            entry->metrics.rightFlat, entry->metrics.rightSpread, entry->metrics.leftFlat);
        if (written < 0 || (size_t)written >= sizeof(screens) - used)
            break;
        used += (size_t)written;
        lastClass = entry->verdict;
        lastMetrics = entry->metrics;
    }
    if (used == 0) {
        screens[0] = '-';
        screens[1] = 0;
    }
    mutexUnlock(&bootUnlockMutex);

    const char* phaseName = phase == BootUnlockPhaseRunning ? "running"
                          : phase == BootUnlockPhaseDone ? "done" : "idle";

    printf("OK enabled=%d mode=%s source=%s reason=%s armed=%d armCleared=%d services=%d phase=%s startMs=%lu endMs=%lu rounds=%u wakePresses=%u homePresses=%u homeSent=%u applicationRuns=%u lastClass=%s lastMetrics=%.2f/%.1f/%.2f schedule=%u,%u,%u,%u,%u,%u,%u screens=%s armPath=%s configPath=%s\n",
        requested ? 1 : 0, bootUnlockModeName(config.mode), source, reason,
        armed ? 1 : 0, cleared ? 1 : 0, servicesReady ? 1 : 0, phaseName,
        (unsigned long)bootUnlockMsAt(startTick), (unsigned long)bootUnlockMsAt(endTick),
        rounds, wakePresses, homePresses, homeSent, applicationRuns,
        screenClassName(lastClass), lastMetrics.rightFlat, lastMetrics.rightSpread,
        lastMetrics.leftFlat,
        config.firstDelayMs, config.wakeAttempts, config.wakeDelayMs, config.verifyDelayMs,
        config.pressGapMs, config.burstPresses, config.maxRounds,
        screens, BOOT_UNLOCK_ARM_PATH, BOOT_UNLOCK_CONFIG_PATH);
}

/* Writes the arm file. Its presence is the whole request; the boot that runs the
 * unlock consumes it, so a request can only ever serve one boot. */
bool bootUnlockRequest(void)
{
    if (!searchStorePrepareDirectory("sdmc:/config/sys-agent"))
        return false;
    FILE* file = fopen(BOOT_UNLOCK_ARM_PATH, "wb");
    if (file == NULL)
        return false;
    /* A timestamp is written for the record only: whether the console really
     * rebooted is decided by the file still being there on the next boot. */
    fprintf(file, "unlock requested by a reboot command\n");
    fclose(file);
    return true;
}

void bootUnlockArmCommand(bool arm)
{
    mutexLock(&bootUnlockMutex);
    BootUnlockMode mode = bootUnlock.config.mode;
    mutexUnlock(&bootUnlockMutex);

    if (arm) {
        if (!bootUnlockRequest()) {
            printf("ERR code=SD_UNAVAILABLE path=%s\n", BOOT_UNLOCK_ARM_PATH);
            return;
        }
        printf("OK arm=enabled path=%s mode=%s\n",
            BOOT_UNLOCK_ARM_PATH, bootUnlockModeName(mode));
        return;
    }

    bool existed = bootUnlockArmFileExists();
    bool removed = !existed || remove(BOOT_UNLOCK_ARM_PATH) == 0;
    if (!removed) {
        printf("ERR code=IO_ERROR action=disarm path=%s\n", BOOT_UNLOCK_ARM_PATH);
        return;
    }
    printf("OK arm=disabled path=%s removed=%d mode=%s\n",
        BOOT_UNLOCK_ARM_PATH, existed ? 1 : 0, bootUnlockModeName(mode));
}

void bootUnlockRunCommand(void)
{
    if (!bootUnlockMutexReady) {
        printf("ERR code=NOT_INITIALISED\n");
        return;
    }
    if (bootUnlock.phase == BootUnlockPhaseRunning) {
        printf("ERR code=BUSY phase=running\n");
        return;
    }

    /* Re-read the schedule: a manual run is what a caller uses to calibrate the
     * file, so it must pick up an edited config without a reboot. */
    BootUnlockConfig config;
    BootUnlockConfigResult result = bootUnlockConfigLoad(BOOT_UNLOCK_CONFIG_PATH, &config);
    if (result == BootUnlockConfigOk || result == BootUnlockConfigNotFound) {
        if (result == BootUnlockConfigNotFound)
            bootUnlockConfigDefaults(&config);
        mutexLock(&bootUnlockMutex);
        bootUnlock.config = config;
        bootUnlock.source = result == BootUnlockConfigOk ? "file" : "default";
        mutexUnlock(&bootUnlockMutex);
    }
    else {
        printf("ERR code=CONFIG %s\n", bootUnlockConfigResultName(result));
        return;
    }

    mutexLock(&bootUnlockMutex);
    BootUnlockConfig active = bootUnlock.config;
    bootUnlock.reason = "manual-run";
    mutexUnlock(&bootUnlockMutex);
    if (!bootUnlockStartWorker()) {
        printf("ERR code=THREAD_START_FAILED\n");
        return;
    }
    printf("OK bootUnlock=started mode=%s firstDelayMs=%u wakeAttempts=%u burstPresses=%u maxRounds=%u\n",
        bootUnlockModeName(active.mode), active.firstDelayMs, active.wakeAttempts,
        active.burstPresses, active.maxRounds);
}
