#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "commands.h"
#include "util.h"
#include "process_memory.h"


//Controller:
bool bControllerIsInitialised = false;
HidDeviceType controllerInitializedType = HidDeviceType_FullKey3;
HiddbgHdlsHandle controllerHandle = { 0 };
HiddbgHdlsDeviceInfo controllerDevice = { 0 };
HiddbgHdlsState controllerState = { 0 };
/* What to do when the player slot is held by one of the console's own
 * controllers: 0 = leave it alone (default: a connected Joy-Con and the virtual
 * pad can both drive the game, and kicking the player's controller leaves it
 * disconnected until it is re-synced), 1 = disconnect that controller and
 * rebuild our device, 2 = also accept the system's "press L+R / A" overlay with
 * a short virtual A. 1/2 are experimental. */
u64 controllerTakeoverMode = 2;
/* The virtual pad always presents itself as a Bluetooth Pro Controller. */
#define CONTROLLER_INTERFACE_TYPE HidNpadInterfaceType_Bluetooth
static Result controllerLastStateError = 0;
static u64 controllerTopology = 0;
static u64 controllerRealPadSampleTick = 0;
static s32 controllerRealPadSample = -1;
static s32 controllerRealPadRailSample = 0;
static u64 controllerTopologySignature(void);
static s32 kickControllerSlotHolders(void);
static void acceptControllerOverlayLocked(void);
static bool parseBluetoothAddress(const char* text, BtdrvAddress* out);
Mutex controllerMutex;

/* One persistent hid:sys session for every controller query. Opening and closing
 * the session per call is fine for a command, but the idle check runs on every
 * main-loop tick (~10/s) and hammering sm/hid:sys that way made commands time
 * out, so the session is opened once and kept. */
static bool controllerHidsysReady = false;

static bool controllerHidsysAcquire(void)
{
    if (controllerHidsysReady)
        return true;

    // Drop a session the service closed on us (libnx only re-opens when the
    // refcount reaches zero, so a plain Initialize() would be a no-op).
    hidsysExit();
    Result rc = hidsysInitialize();
    if (R_FAILED(rc))
        return false;

    controllerHidsysReady = true;
    return true;
}

/* hid:sys closes the whole session when it does not like a request (0xF601).
 * Forget the cached session so the next call re-initialises it instead of
 * failing forever. */
static void controllerHidsysDropOnFailure(Result rc)
{
    if (R_FAILED(rc))
        controllerHidsysReady = false;
}

/* Bluetooth addresses of the console's own controllers that a takeover
 * disconnected. btdrv can then trigger the connection again, which is what the
 * controller itself cannot do after being disconnected (a button press or even
 * SYNC does nothing until it is re-seated on the rail). */
#define CONTROLLER_SAVED_ADDR_MAX 4
static BtdrvAddress controllerSavedAddr[CONTROLLER_SAVED_ADDR_MAX];
static s32 controllerSavedAddrCount = 0;
static s32 controllerReconnectAttempts = 0;
static u64 controllerReconnectNextTick = 0;

static void controllerSaveAddress(HidsysUniquePadId pad)
{
    if (controllerSavedAddrCount >= CONTROLLER_SAVED_ADDR_MAX)
        return;

    BtdrvAddress addr;
    memset(&addr, 0, sizeof addr);
    if (R_FAILED(hidsysGetUniquePadBluetoothAddress(pad, &addr)))
        return;

    for (s32 i = 0; i < controllerSavedAddrCount; i++) {
        if (memcmp(&controllerSavedAddr[i], &addr, sizeof addr) == 0)
            return;
    }
    controllerSavedAddr[controllerSavedAddrCount++] = addr;
    /* A new kick starts a fresh retry budget for the reconnect passes. */
    controllerReconnectAttempts = 0;
    controllerReconnectNextTick = 0;
}

/* btdrv sessions are opened per use: the guard in libnx only re-opens when the
 * refcount drops to zero, and the service closes the session outright when it
 * rejects a request. */
static bool controllerBtdrvAcquire(void)
{
    btdrvExit();
    return R_SUCCEEDED(btdrvInitialize());
}

/* Bring back the controllers a takeover disconnected. They stay paired but never
 * page the console again on their own (a button press, even SYNC, does nothing
 * until they are re-seated on the rail), so the console has to trigger the
 * connection itself. The first trigger often returns a "link busy" error for the
 * second controller of a pair, so this runs from the main loop and retries
 * failed addresses on later passes - never blocking command handling. */
#define CONTROLLER_RECONNECT_RETRIES 3
#define CONTROLLER_RECONNECT_INTERVAL_NS 1500000000ULL

void controllerServiceReconnect(void)
{
    if (controllerSavedAddrCount == 0)
        return;

    u64 now = armGetSystemTick();
    if (controllerReconnectNextTick != 0 && now < controllerReconnectNextTick)
        return;

    if (!controllerBtdrvAcquire()) {
        controllerSavedAddrCount = 0;
        controllerReconnectAttempts = 0;
        controllerReconnectNextTick = 0;
        return;
    }

    BtdrvAddress retry[CONTROLLER_SAVED_ADDR_MAX];
    s32 retryCount = 0;
    for (s32 i = 0; i < controllerSavedAddrCount && i < CONTROLLER_SAVED_ADDR_MAX; i++) {
        Result rc = btdrvTriggerConnection(controllerSavedAddr[i], 5000);
        /* A controller that is already connected also reports an error. */
        if (R_FAILED(rc))
            retry[retryCount++] = controllerSavedAddr[i];
    }
    if (retryCount > 0)
        memcpy(controllerSavedAddr, retry, sizeof(BtdrvAddress) * retryCount);
    controllerSavedAddrCount = retryCount;
    controllerReconnectAttempts++;

    if (controllerSavedAddrCount == 0 || controllerReconnectAttempts >= CONTROLLER_RECONNECT_RETRIES) {
        controllerSavedAddrCount = 0;
        controllerReconnectAttempts = 0;
        controllerReconnectNextTick = 0;
        btdrvExit();
        return;
    }

    u64 intervalTicks = CONTROLLER_RECONNECT_INTERVAL_NS * armGetSystemTickFreq() / 1000000000ULL;
    controllerReconnectNextTick = now + intervalTicks;
}

//Keyboard:
HiddbgKeyboardAutoPilotState dummyKeyboardState = { 0 };

u64 buttonClickSleepTime = 50;
u64 keyPressSleepTime = 25;
u64 pollRate = 17; // polling is linked to screen refresh rate (system UI) or game framerate. Most cases this is 1/60 or 1/30
u32 fingerDiameter = 50;
HiddbgHdlsSessionId sessionId = { 0 };
bool initflag = 0;
u8* workmem = NULL;
size_t workmem_size = 0x1000;

u64 getTitleId(u64 pid) {
    u64 titleId = 0;
    Result rc = pminfoGetProgramId(&titleId, pid);
    if (R_FAILED(rc) && debugResultCodes)
        printf("pminfoGetProgramId: %d\n", rc);
    return titleId;
}

u64 GetTitleVersion(u64 pid) {
    u64 titleV = 0;
    s32 out;

    Result rc = initServiceWithRetry(nsInitialize);
    if (R_FAILED(rc)) {
        /* Return 0 ("version unavailable") instead of an error line:
         * getMetaData() feeds 13 commands, so printing ERR here would corrupt
         * their responses, and 0.0.0 is never a real version for a running
         * title. */
        logDiagnostic("nsInitialize (GetTitleVersion)", rc);
        return 0;
    }

    NsApplicationContentMetaStatus* MetaStatus = malloc(sizeof(NsApplicationContentMetaStatus[100U]));
    if (MetaStatus == NULL) {
        // Handle allocation failure
        printf("Failed to allocate memory for MetaStatus\n");
        nsExit();
        return 0; // or another appropriate error value
    }
    rc = nsListApplicationContentMetaStatus(getTitleId(pid), 0, MetaStatus, 100, &out);
    if (R_FAILED(rc) && debugResultCodes)
        printf("nsListApplicationContentMetaStatus: %d\n", rc);
    for (int i = 0; i < out; i++) {
        if (titleV < MetaStatus[i].version) titleV = MetaStatus[i].version;
    }

    free(MetaStatus);
    nsExit();

    return (titleV / 0x10000);
}

u64 getoutsize(NsApplicationControlData* buf) {
    Result rc = initServiceWithRetry(nsInitialize);
    if (R_FAILED(rc)) {
        printf("ERR code=SERVICE_UNAVAILABLE service=ns result=0x%X\n", rc);
        logDiagnostic("nsInitialize (getoutsize)", rc);
        return 0;
    }
    u64 outsize = 0;
    u64 pid = 0;
    pmdmntGetApplicationProcessId(&pid);
    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, getTitleId(pid), buf, sizeof(NsApplicationControlData), &outsize);
    if (R_FAILED(rc)) {
        printf("nsGetApplicationControlData() failed: 0x%x\n", rc);
    }
    nsExit();
    return outsize;
}

void getBuildID(MetaData* meta, u64 pid) {
    LoaderModuleInfo proc_modules[2];
    s32 numModules = 0;
    Result rc = ldrDmntGetProcessModuleInfo(pid, proc_modules, 2, &numModules);
    if (R_FAILED(rc)) {
        if (debugResultCodes)
            printf("ldrDmntGetProcessModuleInfo: %d\n", rc);
        memset(meta->buildID, 0, 0x20);
        return;
    }

    LoaderModuleInfo* proc_module = 0;
    if (numModules == 2) {
        proc_module = &proc_modules[1];
    }
    else {
        proc_module = &proc_modules[0];
    }
    memcpy(meta->buildID, proc_module->build_id, 0x20);
}

MetaData getMetaData() {
    MetaData meta = { 0 };
    ProcessMemorySession session;
    Result rc = processMemoryOpen(&session, debugResultCodes);
    if (R_FAILED(rc))
        return meta;
    const ProcessMemoryMetadata* metadata = processMemoryGetMetadata(&session);
    meta.main_nso_base = metadata->mainBase;
    meta.heap_base = metadata->heapBase;
    meta.titleID = metadata->titleId;
    meta.titleVersion = GetTitleVersion(metadata->processId);
    memcpy(meta.buildID, metadata->buildId, sizeof(meta.buildID));
    processMemoryClose(&session);
    return meta;
}

bool getIsProgramOpen(u64 id)
{
    u64 pid = 0;
    Result rc = pmdmntGetProcessId(&pid, id);
    if (pid == 0 || R_FAILED(rc))
        return false;

    return true;
}

/* Whether the HDLS device created by initControllerLocked() is still attached.
 * HOS drops the virtual device whenever the controller topology changes
 * (docking, Joy-Con attach/detach, game launch): the handle then stays valid
 * looking to us but every hiddbgSetHdlsState fails with a hid Result and the
 * input silently goes nowhere. */
static bool controllerDeviceIsAttachedLocked(void)
{
    if (!bControllerIsInitialised || controllerHandle.handle == 0)
        return false;

    bool attached = false;
    Result rc = hiddbgIsHdlsVirtualDeviceAttached(sessionId, controllerHandle, &attached);
    if (R_FAILED(rc)) {
        if (debugResultCodes)
            printf("hiddbgIsHdlsVirtualDeviceAttached: 0x%X\n", rc);
        return false;
    }
    return attached;
}

/* Releases the work buffer and forgets the handle. Safe to call when nothing is
 * attached; never leaves a half-initialised session behind. */
static void teardownControllerLocked(void)
{
    if (workmem != NULL) {
        if (bControllerIsInitialised) {
            Result rc = hiddbgDetachHdlsVirtualDevice(controllerHandle);
            if (R_FAILED(rc) && debugResultCodes)
                printf("hiddbgDetachHdlsVirtualDevice: %d\n", rc);
        }
        Result rc = hiddbgReleaseHdlsWorkBuffer(sessionId);
        if (R_FAILED(rc) && debugResultCodes)
            printf("hiddbgReleaseHdlsWorkBuffer: %d\n", rc);
        hiddbgExit();
        free(workmem);
        workmem = NULL;
    }

    initflag = 0;
    bControllerIsInitialised = false;
    controllerHandle.handle = 0;
    sessionId.id = 0;
    controllerRealPadSampleTick = 0;
}

static void initControllerLocked()
{
    if (bControllerIsInitialised) return;
    //taken from switchexamples github
    Result rc = hiddbgInitialize();

    //old
    //if (R_FAILED(rc) && debugResultCodes)
    //printf("hiddbgInitialize: %d\n", rc);

    //new
    if (R_FAILED(rc) && debugResultCodes) {
        printf("hiddbgInitialize(): 0x%x\n", rc);
    }
    else {
        workmem = aligned_alloc(0x1000, workmem_size);
        if (workmem) initflag = 1;
        else printf("workmem alloc failed\n");
    }

    // Set the controller type to Pro-Controller, and set the npadInterfaceType.
    controllerDevice.deviceType = controllerInitializedType;
    controllerDevice.npadInterfaceType = CONTROLLER_INTERFACE_TYPE;
    // Set the controller colors. The grip colors are for Pro-Controller on [9.0.0+].
    controllerDevice.singleColorBody = RGBA8_MAXALPHA(255, 255, 255);
    controllerDevice.singleColorButtons = RGBA8_MAXALPHA(0, 0, 0);
    controllerDevice.colorLeftGrip = RGBA8_MAXALPHA(230, 255, 0);
    controllerDevice.colorRightGrip = RGBA8_MAXALPHA(0, 40, 20);

    // Setup example controller state.
    controllerState.battery_level = 4; // Set battery charge to full.
    // Keep the caller's intended state: rebuilds happen behind the scenes (see
    // controllerRefreshIfIdle) and must not release a button the caller is
    // still holding. detachController clears it explicitly instead.

    rc = hiddbgAttachHdlsWorkBuffer(&sessionId, workmem, workmem_size);
    if (R_FAILED(rc) && debugResultCodes)
        printf("hiddbgAttachHdlsWorkBuffer: %d\n", rc);
    rc = hiddbgAttachHdlsVirtualDevice(&controllerHandle, &controllerDevice);
    if (R_FAILED(rc) && debugResultCodes)
        printf("hiddbgAttachHdlsVirtualDevice: %d\n", rc);
    // HOS and the running game need a moment to pick up a freshly attached
    // device. Without this settle time the first state write after a rebuild is
    // accepted but not routed, so the first click after a topology change is
    // silently dropped.
    svcSleepThread(150 * 1e+6L);
    rc = hiddbgSetHdlsState(controllerHandle, &controllerState);
    if (R_FAILED(rc) && debugResultCodes)
        printf("hiddbgSetHdlsState: %d\n", rc);
    bControllerIsInitialised = true;
    // Our own pad changed the pad list: resample it for the idle check.
    controllerRealPadSampleTick = 0;
    // Remember the layout this device was built for, so the idle refresh only
    // rebuilds it after a real topology change.
    controllerTopology = controllerTopologySignature();
}

/* Makes sure a usable virtual device exists, rebuilding it when HOS dropped the
 * previous one. Returns false when it could not be attached at all. */
static bool ensureControllerLocked(void)
{
    if (controllerDeviceIsAttachedLocked())
        return true;

    /* Fresh attach. If one of the console's own controllers holds player 1, the
     * user's policy is to take the slot over instead of fighting for it: the
     * virtual pad is a disposable input source and must win while it is used. */
    s32 kicked = 0;
    if (controllerTakeoverMode >= 1)
        kicked = kickControllerSlotHolders();

    teardownControllerLocked();
    initControllerLocked();

    bool attached = controllerDeviceIsAttachedLocked();
    if (attached && kicked > 0 && controllerTakeoverMode >= 2)
        acceptControllerOverlayLocked();
    return attached;
}

void detachController()
{
    mutexLock(&controllerMutex);
    teardownControllerLocked();
    // Explicit detach is a "start over" request: never carry held buttons or a
    // stick position into the next device, stale bits would suppress the next
    // press edge.
    controllerState.buttons = 0;
    controllerState.analog_stick_l.x = 0;
    controllerState.analog_stick_l.y = 0;
    controllerState.analog_stick_r.x = 0;
    controllerState.analog_stick_r.y = 0;
    mutexUnlock(&controllerMutex);
}

/* A virtual device that survived a controller topology change (dock,
 * Joy-Con attach/detach, game launch) is not reliably routed any more, and the
 * failure is silent: hiddbgSetHdlsState keeps reporting success while the game
 * ignores the input. Presenting the device as newly connected again makes both
 * HOS and the running game pick it up, so rebuild it when input resumes after a
 * pause *and* the controller layout is not the one the device was built for.
 *
 * Rebuilding creates a new HDLS device (and a new unique pad id), so it is
 * deliberately rare: it needs 0.5 s of idle to be considered, is skipped while
 * the layout signature is unchanged, and is rate limited as a safety net. */
#define CONTROLLER_REFRESH_IDLE_NS 500000000ULL          /* 0.5 s */
#define CONTROLLER_REFRESH_INTERVAL_NS 5000000000ULL     /* 5 s */

static u64 controllerLastInputTick = 0;
static u64 controllerLastRefreshTick = 0;

/* Seconds of "no input command at all" after which the virtual device is
 * released so the console's own controllers can take the player slot back.
 * Only used while a real controller is connected: with no controller of its own
 * the console has nothing to hand the player slot to, and releasing would just
 * leave the game asking for a controller. 0 disables the release. */
u64 controllerIdleReleaseSeconds = 1;

/* Set when a takeover disconnected one of the console's own controllers: the
 * player clearly has a controller, so the slot must be handed back after use
 * even though that controller is temporarily missing from the pad list. */
static bool controllerYieldAfterUse = false;

/* True while a clickSeq is being executed: the sequence must not be interrupted
 * by the idle release. */
static bool controllerSequenceActive = false;

/* Snapshot of the console's own controllers: how many pads exist besides our
 * virtual device, and how many of them are the two Joy-Cons sitting on the
 * rails. Rail Joy-Cons are the handheld pad; while docked they cannot drive the
 * game, so handing the player slot back to them would just leave the game with
 * no controller and make it pop its "connect a controller" prompt. */
typedef struct {
    s32 otherPads;
    s32 railPads;
} ControllerPadSample;

static ControllerPadSample controllerPadSampleNow(void)
{
    ControllerPadSample sample = { -1, 0 };
    if (!controllerHidsysAcquire())
        return sample;

    HidsysUniquePadId pads[32];
    s32 total = 0;
    memset(pads, 0, sizeof pads);
    Result listRc = hidsysGetUniquePadIds(pads, 32, &total);
    controllerHidsysDropOnFailure(listRc);
    if (R_SUCCEEDED(listRc) && total >= 0) {
        s32 ours = bControllerIsInitialised ? 1 : 0;
        sample.otherPads = total - ours;
        if (sample.otherPads < 0)
            sample.otherPads = 0;
    }

    bool rail = false;
    Result railRc = hidsysIsJoyConAttachedOnAllRail(&rail);
    controllerHidsysDropOnFailure(railRc);
    if (R_SUCCEEDED(railRc) && rail)
        sample.railPads = 2;

    return sample;
}

/* The idle check runs on every main-loop tick; the pad list only needs to be
 * sampled a couple of times per second. */
#define CONTROLLER_PAD_SAMPLE_NS 500000000ULL

static ControllerPadSample controllerPadSampleCached(void)
{
    u64 now = armGetSystemTick();
    if (controllerRealPadSampleTick != 0 &&
        armTicksToNs(now - controllerRealPadSampleTick) < CONTROLLER_PAD_SAMPLE_NS)
        return (ControllerPadSample){ controllerRealPadSample, controllerRealPadRailSample };

    ControllerPadSample sample = controllerPadSampleNow();
    controllerRealPadSample = sample.otherPads;
    controllerRealPadRailSample = sample.railPads;
    controllerRealPadSampleTick = now;
    return sample;
}

void controllerServiceIdle(void)
{
    if (!bControllerIsInitialised)
        return;

    if (controllerIdleReleaseSeconds == 0)
        return;

    /* Hand the player slot back only when the player has a controller that can
     * actually take it over. Releasing with nothing else connected - or with
     * only the rail Joy-Cons attached while docked - is what made the game pop
     * its "connect a controller" prompt out of nowhere. A controller we kicked
     * during a takeover counts as well, even though it is off the pad list
     * until the player presses it again. */
    ControllerPadSample sample = controllerPadSampleCached();
    bool otherController = sample.otherPads > sample.railPads;
    if (!otherController && !controllerYieldAfterUse)
        return;

    u64 now = armGetSystemTick();
    mutexLock(&controllerMutex);
    bool idle = controllerLastInputTick == 0 ||
                armTicksToNs(now - controllerLastInputTick) >=
                    controllerIdleReleaseSeconds * 1000000000ULL;
    bool neutralState = controllerState.buttons == 0 &&
                        controllerState.analog_stick_l.x == 0 && controllerState.analog_stick_l.y == 0 &&
                        controllerState.analog_stick_r.x == 0 && controllerState.analog_stick_r.y == 0;
    if (bControllerIsInitialised && idle && !controllerSequenceActive && neutralState) {
        // Let the player's own controller take player 1 back; the game asks for
        // a controller again and the player accepts with its L+R.
        teardownControllerLocked();
        controllerTopology = 0;
        controllerYieldAfterUse = false;
    }
    mutexUnlock(&controllerMutex);
}

/* Signature of the controller layout as HOS reports it: rail state, handheld
 * hids flag and the number of connected pads. Every topology change observed on
 * hardware so far (docking, Joy-Con attach/detach, game launch) moves at least
 * one of them. Returns 0 when hidsys does not answer. */
static u64 controllerTopologySignature(void)
{
    if (!controllerHidsysAcquire())
        return 0;

    u64 signature = 0x100000000ULL;
    bool rail = false;
    if (R_SUCCEEDED(hidsysIsJoyConAttachedOnAllRail(&rail)) && rail)
        signature |= 1;
    bool handheld = false;
    if (R_SUCCEEDED(hidsysIsHandheldHidsEnabled(&handheld)) && handheld)
        signature |= 2;

    HidsysUniquePadId pads[16];
    s32 total = 0;
    if (R_SUCCEEDED(hidsysGetUniquePadIds(pads, 16, &total)) && total >= 0)
        signature |= ((u64)total & 0xFF) << 8;

    return signature;
}

void controllerRefreshIfIdle(void)
{
    u64 now = armGetSystemTick();

    mutexLock(&controllerMutex);
    bool idle = controllerLastInputTick != 0 &&
                armTicksToNs(now - controllerLastInputTick) >= CONTROLLER_REFRESH_IDLE_NS;
    bool rateLimited = controllerLastRefreshTick != 0 &&
                       armTicksToNs(now - controllerLastRefreshTick) < CONTROLLER_REFRESH_INTERVAL_NS;
    controllerLastInputTick = now;

    if (idle && !rateLimited && bControllerIsInitialised) {
        u64 signature = controllerTopologySignature();
        if (signature != 0 && signature != controllerTopology) {
            controllerTopology = signature;
            controllerLastRefreshTick = now;
            teardownControllerLocked();
        }
    }
    mutexUnlock(&controllerMutex);
}

/* --- Virtual controller diagnostics and player-slot experiments ----------- */

static const char* controllerInterfaceName(u8 value)
{
    switch (value) {
        case HidNpadInterfaceType_Bluetooth: return "bluetooth";
        case HidNpadInterfaceType_Rail:      return "rail";
        case HidNpadInterfaceType_USB:       return "usb";
        default:                             return "other";
    }
}

static bool controllerEntryIsEmpty(const HiddbgHdlsNpadAssignmentEntry* entry)
{
    return entry->handle.handle == 0 && entry->unk_x8 == 0 && entry->unk_xc == 0 &&
           entry->unk_x10 == 0 && entry->unk_x18 == 0;
}

/* HdlsNpadAssignment is the "which HDLS handle owns which npad slot" table of
 * hid:dbg. Past the struct layout it is undocumented, so every use prints the
 * raw table plus the Result of every call. */
static Result captureControllerNpadAssignmentLocked(HiddbgHdlsNpadAssignment* assignment)
{
    memset(assignment, 0, sizeof *assignment);
    return hiddbgDumpHdlsNpadAssignmentState(sessionId, assignment);
}

static void printControllerNpadAssignment(const char* tag, const HiddbgHdlsNpadAssignment* assignment)
{
    printf("%s total=%d\n", tag, assignment->total_entries);
    for (s32 i = 0; i < 0x10; i++) {
        const HiddbgHdlsNpadAssignmentEntry* entry = &assignment->entries[i];
        bool ours = controllerHandle.handle != 0 && entry->handle.handle == controllerHandle.handle;
        /* Rows past total_entries are HOS-internal and can be uninitialised, so
         * only print them when they carry our own handle. */
        if (controllerEntryIsEmpty(entry) || (!ours && i >= assignment->total_entries))
            continue;
        printf("  npad[%02d] handle=%016lX unk8=%08X unkC=%08X unk10=%016lX unk18=%02X%s\n",
               i, entry->handle.handle, entry->unk_x8, entry->unk_xc, entry->unk_x10,
               entry->unk_x18,
               ours ? " ours" : "");
    }
}

/* Lists every controller hid:dbg knows about, virtual or not. */
static void dumpControllerStateListLocked(const char* tag)
{
    HiddbgHdlsStateList list;
    memset(&list, 0, sizeof list);

    Result rc = hiddbgDumpHdlsStates(sessionId, &list);
    if (R_FAILED(rc)) {
        printf("%s rc=0x%08X\n", tag, rc);
        return;
    }

    printf("%s total=%d\n", tag, list.total_entries);
    for (s32 i = 0; i < 0x10; i++) {
        const HiddbgHdlsStateListEntry* entry = &list.entries[i];
        bool ours = controllerHandle.handle != 0 && entry->handle.handle == controllerHandle.handle;
        if (entry->handle.handle == 0 || (!ours && i >= list.total_entries))
            continue;
        printf("  [%02d] handle=%016lX type=0x%02X iface=%d buttons=%016lX flags=%08X battery=%d%s\n",
               i, entry->handle.handle, entry->device.deviceType,
               entry->device.npadInterfaceType, entry->state.buttons, entry->state.flags,
               entry->state.battery_level,
               ours ? " ours" : "");
    }
}

/* hidsys side: which unique pad is assigned to which player slot. */
static void dumpControllerOwners(const char* tag)
{
    if (!controllerHidsysAcquire()) {
        printf("%s hidsysInitialize=failed\n", tag);
        return;
    }

    printf("%s\n", tag);

    u32 lastActive = 0xFFFFFFFFu;
    if (R_SUCCEEDED(hidsysGetLastActiveNpad(&lastActive)))
        printf("  lastActiveNpad=%u\n", lastActive);

    bool railAttached = false;
    if (R_SUCCEEDED(hidsysIsJoyConAttachedOnAllRail(&railAttached)))
        printf("  joyConAttachedOnAllRail=%d\n", railAttached);

    bool handheldHids = false;
    if (R_SUCCEEDED(hidsysIsHandheldHidsEnabled(&handheldHids)))
        printf("  handheldHidsEnabled=%d\n", handheldHids);

    /* Every pad HOS currently knows about. Dead HDLS devices linger in the
     * per-npad tables, so the total count is the leak indicator to watch. */
    HidsysUniquePadId allPads[32];
    s32 allTotal = 0;
    memset(allPads, 0, sizeof allPads);
    if (R_SUCCEEDED(hidsysGetUniquePadIds(allPads, 32, &allTotal))) {
        printf("  uniquePads=%d\n", allTotal);
        for (s32 i = 0; i < allTotal; i++) {
            HidNpadInterfaceType iface = (HidNpadInterfaceType)0;
            u64 number = 0;
            Result ifaceRc = hidsysGetUniquePadInterface(allPads[i], &iface);
            Result numberRc = hidsysGetUniquePadControllerNumber(allPads[i], &number);
            printf("  pad=%016lX iface=%d ifaceRc=0x%08X number=%lu numberRc=0x%08X\n",
                   allPads[i].id, (u8)iface, ifaceRc, (u64)number, numberRc);
        }
    }

    const HidNpadIdType ids[] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
        HidNpadIdType_Other, HidNpadIdType_Handheld,
    };

    for (size_t n = 0; n < sizeof(ids) / sizeof(ids[0]); n++) {
        HidsysUniquePadId pads[8];
        s32 total = 0;
        memset(pads, 0, sizeof pads);

        Result listRc = hidsysGetUniquePadsFromNpad(ids[n], pads, 8, &total);
        if (R_FAILED(listRc)) {
            printf("  npad=%u listRc=0x%08X\n", (u32)ids[n], listRc);
            continue;
        }
        if (total == 0) {
            printf("  npad=%u free\n", (u32)ids[n]);
            continue;
        }

        for (s32 i = 0; i < total; i++) {
            u64 number = 0;
            HidNpadInterfaceType iface = (HidNpadInterfaceType)0;
            Result numberRc = hidsysGetUniquePadControllerNumber(pads[i], &number);
            Result ifaceRc = hidsysGetUniquePadInterface(pads[i], &iface);
            printf("  npad=%u pad=%016lX number=%lu numberRc=0x%08X iface=%d ifaceRc=0x%08X\n",
                   (u32)ids[n], pads[i].id, (u64)number, numberRc, (u8)iface, ifaceRc);
        }
    }

}

void controllerStatusCommand(void)
{
    mutexLock(&controllerMutex);

    /* Read-only: never create the virtual device just to inspect it. */
    bool initialised = bControllerIsInitialised;
    s32 slot = -1;
    Result assignmentRc = 0;

    if (initialised) {
        HiddbgHdlsNpadAssignment assignment;
        memset(&assignment, 0, sizeof assignment);
        assignmentRc = hiddbgDumpHdlsNpadAssignmentState(sessionId, &assignment);
        if (R_SUCCEEDED(assignmentRc) && controllerHandle.handle != 0) {
            for (s32 i = 0; i < 0x10; i++) {
                if (assignment.entries[i].handle.handle == controllerHandle.handle) {
                    slot = i;
                    break;
                }
            }
        }
    }

    // Player 1 owner as hidsys sees it: the slot the game actually reads.
    u64 npad0 = 0;
    bool npad0Queried = false;
    if (controllerHidsysAcquire()) {
        HidsysUniquePadId pads[8];
        s32 total = 0;
        memset(pads, 0, sizeof pads);
        Result ownerRc = hidsysGetUniquePadsFromNpad(HidNpadIdType_No1, pads, 8, &total);
        if (R_SUCCEEDED(ownerRc)) {
            npad0Queried = true;
            if (total > 0)
                npad0 = pads[0].id;
        }
    }

    printf("OK initialised=%d handle=%016lX session=%016lX attached=%d deviceType=0x%02X interface=%d(%s) idleRelease=%lu takeover=%lu slot=%d assignmentRc=0x%08X lastStateError=0x%08X npad0=",
           initialised, controllerHandle.handle, sessionId.id,
           initialised ? controllerDeviceIsAttachedLocked() : false,
           (u8)controllerInitializedType, (u8)CONTROLLER_INTERFACE_TYPE,
           controllerInterfaceName((u8)CONTROLLER_INTERFACE_TYPE),
           controllerIdleReleaseSeconds, controllerTakeoverMode,
           slot, assignmentRc, controllerLastStateError);
    if (!npad0Queried)
        printf("NA\n");
    else if (npad0 == 0)
        printf("free\n");
    else
        printf("%016lX\n", npad0);
    mutexUnlock(&controllerMutex);
}

void controllerDumpCommand(void)
{
    mutexLock(&controllerMutex);

    /* Read-only: never create the virtual device just to inspect it. The
     * hid:dbg tables need our work buffer session, so they are skipped when no
     * device is attached; the hidsys view below still works. */
    bool initialised = bControllerIsInitialised;
    printf("OK initialised=%d handle=%016lX session=%016lX attached=%d deviceType=0x%02X interface=%d(%s) idleRelease=%lu takeover=%lu topology=0x%lX lastStateError=0x%08X\n",
           initialised, controllerHandle.handle, sessionId.id,
           initialised ? controllerDeviceIsAttachedLocked() : false,
           (u8)controllerInitializedType, (u8)CONTROLLER_INTERFACE_TYPE,
           controllerInterfaceName((u8)CONTROLLER_INTERFACE_TYPE),
           controllerIdleReleaseSeconds, controllerTakeoverMode,
           controllerTopology, controllerLastStateError);

    if (initialised) {
        HiddbgHdlsNpadAssignment assignment;
        Result assignmentRc = captureControllerNpadAssignmentLocked(&assignment);
        if (R_SUCCEEDED(assignmentRc))
            printControllerNpadAssignment("npadAssignment", &assignment);
        else
            printf("npadAssignment rc=0x%08X\n", assignmentRc);

        dumpControllerStateListLocked("hdlsStates");
    }
    mutexUnlock(&controllerMutex);

    dumpControllerOwners("hidsysNpads");
    printf("END controllerDump\n");
    fflush(stdout);
}

/* Experimental: frees a player slot by disconnecting the real controller that
 * holds it, so the virtual device can be assigned there. Never used
 * automatically - it takes the player's own controller away. */
void controllerKickCommand(const char* arg)
{
    s32 npadId = (s32)parseStringToInt((char*)arg);
    if (npadId < 0 || npadId > 7) {
        printf("ERR controllerKick range=0-7 args=1\n");
        return;
    }

    if (!controllerHidsysAcquire()) {
        printf("ERR controllerKick hidsysInitialize=failed\n");
        return;
    }

    printf("WARN controllerKick disconnects the real controller(s) holding that player slot.\n");

    HidsysUniquePadId pads[8];
    s32 total = 0;
    memset(pads, 0, sizeof pads);

    Result rc = hidsysGetUniquePadsFromNpad((HidNpadIdType)npadId, pads, 8, &total);
    printf("OK npad=%d pads=%d listRc=0x%08X\n", npadId, total, rc);
    for (s32 i = 0; i < total; i++) {
        controllerSaveAddress(pads[i]);
        Result disconnectRc = hidsysDisconnectUniquePad(pads[i]);
        printf("  pad=%016lX disconnectRc=0x%08X\n", pads[i].id, disconnectRc);
    }
    printf("END controllerKick\n");
    fflush(stdout);

}

/* Pulls the controllers a takeover disconnected back, using their Bluetooth
 * addresses: btdrvTriggerConnection is the same "connect to this device"
 * primitive the console's own controller manager uses. Prints the paired-info
 * and trigger results per address plus the pad list afterwards. */
void controllerReconnectCommand(const char* arg, const char* addrArg)
{
    u16 timeout = arg != NULL ? (u16)parseStringToInt((char*)arg) : 0;
    if (timeout == 0)
        timeout = 5000;

    /* An explicit address overrides the remembered list (useful when the
     * controller was never kicked by this boot, e.g. after a reboot). */
    bool explicitAddress = false;
    if (addrArg != NULL && addrArg[0] != 0) {
        BtdrvAddress addr;
        memset(&addr, 0, sizeof addr);
        if (!parseBluetoothAddress(addrArg, &addr)) {
            printf("ERR controllerReconnect invalidAddress=%s\n", addrArg);
            printf("END controllerReconnect\n");
            fflush(stdout);
            return;
        }
        controllerSavedAddr[0] = addr;
        controllerSavedAddrCount = 1;
        controllerReconnectAttempts = 0;
        controllerReconnectNextTick = 0;
        explicitAddress = true;
    }

    printf("OK saved=%d timeout=%u\n", controllerSavedAddrCount, timeout);
    for (s32 i = 0; i < controllerSavedAddrCount; i++) {
        const u8* a = controllerSavedAddr[i].address;
        printf("  addr[%d]=%02X:%02X:%02X:%02X:%02X:%02X\n",
               i, a[0], a[1], a[2], a[3], a[4], a[5]);
    }

    if (controllerSavedAddrCount == 0) {
        printf("ERR controllerReconnect noSavedAddresses (run controllerKick first, or pass an address)\n");
        printf("END controllerReconnect\n");
        fflush(stdout);
        return;
    }

    if (!controllerBtdrvAcquire()) {
        printf("ERR controllerReconnect btdrvInitialize=failed\n");
        printf("END controllerReconnect\n");
        fflush(stdout);
        return;
    }

    for (s32 i = 0; i < controllerSavedAddrCount; i++) {
        SetSysBluetoothDevicesSettings info;
        memset(&info, 0, sizeof info);
        Result pairedRc = btdrvGetPairedDeviceInfo(controllerSavedAddr[i], &info);
        Result triggerRc = btdrvTriggerConnection(controllerSavedAddr[i], timeout);
        printf("  addr[%d] pairedRc=0x%08X triggerRc=0x%08X\n", i, pairedRc, triggerRc);
        fflush(stdout);
    }

    if (explicitAddress) {
        controllerSavedAddrCount = 0;
        controllerReconnectAttempts = 0;
        controllerReconnectNextTick = 0;
        btdrvExit();
        printf("END controllerReconnect\n");
        fflush(stdout);
        return;
    }

    svcSleepThread(5 * 1e+9L);

    if (controllerHidsysAcquire()) {
        HidsysUniquePadId pads[32];
        s32 total = 0;
        memset(pads, 0, sizeof pads);
        Result listRc = hidsysGetUniquePadIds(pads, 32, &total);
        controllerHidsysDropOnFailure(listRc);
        if (R_SUCCEEDED(listRc)) {
            printf("padsAfter=%d\n", total);
            for (s32 i = 0; i < total; i++)
                printf("  after pad=%016lX\n", pads[i].id);
        }
        else {
            printf("padsAfter=NA rc=0x%08X\n", listRc);
        }
    }
    printf("END controllerReconnect\n");
    fflush(stdout);
}

/* Lists the console's paired Bluetooth devices with their addresses, so "still
 * paired" can be checked independently of "currently connected". */
void controllerPairedDevicesCommand(void)
{
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        printf("ERR controllerPairedDevices setsysInitialize=0x%08X\n", rc);
        return;
    }

    SetSysBluetoothDevicesSettings devices[8];
    s32 total = 0;
    memset(devices, 0, sizeof devices);
    Result listRc = setsysGetBluetoothDevicesSettings(&total, devices, 8);
    printf("OK listRc=0x%08X total=%d\n", listRc, total);
    for (s32 i = 0; i < total && i < 8; i++) {
        const u8* a = devices[i].addr.address;
        char name[0x40];
        memcpy(name, devices[i].name2, sizeof name - 1);
        name[sizeof name - 1] = 0;
        printf("  [%d] addr=%02X:%02X:%02X:%02X:%02X:%02X type=0x%02X linkKey=%d name=%s\n",
               i, a[0], a[1], a[2], a[3], a[4], a[5], devices[i].device_type,
               devices[i].link_key_present, name);
    }
    printf("END controllerPairedDevices\n");
    fflush(stdout);

    setsysExit();
}

/* Parses "AA:BB:CC:DD:EE:FF" or "AABBCCDDEEFF" into a Bluetooth address. */
static bool parseBluetoothAddress(const char* text, BtdrvAddress* out)
{
    u8 bytes[6];
    s32 count = 0;
    for (const char* p = text; *p != 0 && count < 6; p++) {
        if (*p == ':' || *p == '-')
            continue;
        s32 hi = -1, lo = -1;
        for (s32 nibble = 0; nibble < 2; nibble++, p++) {
            if (*p == 0)
                return false;
            char c = *p;
            s32 value;
            if (c >= '0' && c <= '9') value = c - '0';
            else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
            else return false;
            if (nibble == 0) hi = value; else lo = value;
        }
        bytes[count++] = (u8)((hi << 4) | lo);
    }
    if (count != 6)
        return false;
    memcpy(out->address, bytes, sizeof bytes);
    return true;
}

void poke(u64 offset, u64 size, u8* val)
{
    ProcessMemorySession session;
    Result rc = processMemoryOpen(&session, debugResultCodes);
    if (R_SUCCEEDED(rc)) {
        rc = processMemoryWrite(&session, val, offset, size);
        processMemoryClose(&session);
    }
    if (R_FAILED(rc) && debugResultCodes)
        printf("processMemoryWrite: %d\n", rc);
}

void peek(u64 offset, u64 size)
{
    u8* out = malloc(sizeof(u8) * size);
    if (out == NULL) {
        printf("\n");
        return;
    }
    ProcessMemorySession session;
    Result rc = processMemoryOpen(&session, debugResultCodes);
    if (R_SUCCEEDED(rc))
        rc = processMemoryRead(&session, out, offset, size);
    if (R_FAILED(rc))
    {
        printf("\n");
        if (session.open)
            processMemoryClose(&session);
        free(out);
        return;
    }

    u64 i;
    for (i = 0; i < size; i++)
    {
        printf("%02X", out[i]);
    }
    printf("\n");
    processMemoryClose(&session);
    free(out);
}

void peekInfinite(u64 offset, u64 size)
{
    u64 sizeRemainder = size;
    u64 totalFetched = 0;
    u64 i;
    u8* out = malloc(sizeof(u8) * MAX_LINE_LENGTH);
    if (out == NULL) {
        printf("\n");
        return;
    }

    ProcessMemorySession session;
    Result rc = processMemoryOpen(&session, debugResultCodes);
    if (R_FAILED(rc)) {
        printf("\n");
        free(out);
        return;
    }
    while (sizeRemainder > 0)
    {
        u64 thisBuffersize = sizeRemainder > MAX_LINE_LENGTH ? MAX_LINE_LENGTH : sizeRemainder;
        sizeRemainder -= thisBuffersize;
        rc = processMemoryRead(&session, out, offset + totalFetched, thisBuffersize);
        if (R_FAILED(rc))
        {
            printf("\n");
            processMemoryClose(&session);
            free(out);
            return;
        }

        for (i = 0; i < thisBuffersize; i++)
        {
            printf("%02X", out[i]);
        }

        totalFetched += thisBuffersize;
    }
    printf("\n");
    processMemoryClose(&session);
    free(out);
}

void peekMulti(u64* offset, u64* size, u64 count)
{
    u64 totalSize = 0;
    for (int i = 0; i < count; i++)
        totalSize += size[i];

    u8* out = malloc(sizeof(u8) * totalSize);
    u64 ofs = 0;
    ProcessMemorySession session;
    Result rc = processMemoryOpen(&session, debugResultCodes);
    if (R_FAILED(rc)) {
        printf("\n");
        free(out);
        return;
    }
    for (int i = 0; i < count; i++)
    {
        rc = processMemoryRead(&session, out + ofs, offset[i], size[i]);
        if (R_FAILED(rc))
        {
            printf("\n");
            processMemoryClose(&session);
            free(out);
            return;
        }
        ofs += size[i];
    }

    u64 i;
    for (i = 0; i < totalSize; i++)
    {
        printf("%02X", out[i]);
    }
    printf("\n");
    processMemoryClose(&session);
    free(out);
}

void click(HidNpadButton btn)
{
    press(btn);
    svcSleepThread(buttonClickSleepTime * 1e+6L);
    release(btn);
}

/* Takes player 1 away from the console's own controller by disconnecting it.
 * Only used when HOS has already refused our state write, i.e. the virtual
 * device is attached but that controller holds the slot we need. Returns the
 * number of pads disconnected (0 when nobody held the slot). */
static s32 kickControllerSlotHolders(void)
{
    if (!controllerHidsysAcquire())
        return 0;

    HidsysUniquePadId pads[8];
    s32 total = 0;
    memset(pads, 0, sizeof pads);
    if (R_FAILED(hidsysGetUniquePadsFromNpad(HidNpadIdType_No1, pads, 8, &total)) || total <= 0)
        return 0;

    s32 kicked = 0;
    for (s32 i = 0; i < total; i++) {
        controllerSaveAddress(pads[i]);
        if (R_SUCCEEDED(hidsysDisconnectUniquePad(pads[i])))
            kicked++;
    }
    if (kicked > 0) {
        // Remember that the player has a controller: the slot has to go back
        // after this burst even though the controller is offline for now.
        controllerYieldAfterUse = true;
        controllerRealPadSampleTick = 0;
    }
    return kicked;
}

/* The system shows its "press L+R, then A" controller overlay as soon as the
 * player's own controller disappears - which is exactly what kicking it causes.
 * A short A confirms that overlay so the game keeps running. Only sent right
 * after a kick, where the overlay is expected. */
static void acceptControllerOverlayLocked(void)
{
    controllerState.buttons |= HidNpadButton_A;
    hiddbgSetHdlsState(controllerHandle, &controllerState);
    svcSleepThread(buttonClickSleepTime * 1e+6L);
    controllerState.buttons &= ~HidNpadButton_A;
    hiddbgSetHdlsState(controllerHandle, &controllerState);
}

/* Pushes the current virtual-controller state to HOS.
 *
 * HOS refuses the write while the player slot belongs to one of the console's
 * own controllers. In that case (and only then) the slot is taken away from it,
 * the device is rebuilt so HOS routes it again, and the write is retried; a
 * failure that still persists is reported once instead of silently dropping
 * the input. */
static void writeControllerStateLocked(void)
{
    ensureControllerLocked();

    Result rc = hiddbgSetHdlsState(controllerHandle, &controllerState);
    if (R_FAILED(rc)) {
        s32 kicked = 0;
        if (controllerTakeoverMode >= 1)
            kicked = kickControllerSlotHolders();
        teardownControllerLocked();
        initControllerLocked();
        rc = hiddbgSetHdlsState(controllerHandle, &controllerState);
        if (R_SUCCEEDED(rc) && kicked > 0 && controllerTakeoverMode >= 2)
            acceptControllerOverlayLocked();
    }

    if (R_FAILED(rc)) {
        if (rc != controllerLastStateError) {
            controllerLastStateError = rc;
            printf("ERR controllerState result=0x%08X attached=%d takeover=%lu\n",
                   rc, controllerDeviceIsAttachedLocked(), controllerTakeoverMode);
        }
    }
    else {
        controllerLastStateError = 0;
    }
}

void press(HidNpadButton btn)
{
    mutexLock(&controllerMutex);
    controllerState.buttons |= btn;
    writeControllerStateLocked();
    mutexUnlock(&controllerMutex);
}

void release(HidNpadButton btn)
{
    mutexLock(&controllerMutex);
    controllerState.buttons &= ~btn;
    writeControllerStateLocked();
    mutexUnlock(&controllerMutex);
}

void setStickState(int side, int dxVal, int dyVal)
{
    mutexLock(&controllerMutex);
    if (side == JOYSTICK_LEFT)
    {
        controllerState.analog_stick_l.x = dxVal;
        controllerState.analog_stick_l.y = dyVal;
    }
    else
    {
        controllerState.analog_stick_r.x = dxVal;
        controllerState.analog_stick_r.y = dyVal;
    }
    writeControllerStateLocked();
    mutexUnlock(&controllerMutex);
}

void reverseArray(u8* arr, int start, int end)
{
    int temp;
    while (start < end)
    {
        temp = arr[start];
        arr[start] = arr[end];
        arr[end] = temp;
        start++;
        end--;
    }
}

u64 followMainPointer(s64* jumps, size_t count)
{
    u64 offset;
    u64 size = sizeof offset;
    u8* out = malloc(size);
    if (out == NULL || count == 0) {
        free(out);
        return 0;
    }

    ProcessMemorySession session;
    Result rc = processMemoryOpen(&session, debugResultCodes);
    if (R_SUCCEEDED(rc)) {
        const ProcessMemoryMetadata* metadata = processMemoryGetMetadata(&session);
        rc = processMemoryRead(&session, out, metadata->mainBase + jumps[0], size);
    }
    if (R_FAILED(rc))
    {
        if (session.open)
            processMemoryClose(&session);
        free(out);
        return 0;
    }
    offset = *(u64*)out;

    int i;
    for (i = 1; i < count; ++i)
    {
        rc = processMemoryRead(&session, out, offset + jumps[i], size);
        if (R_FAILED(rc))
        {
            processMemoryClose(&session);
            free(out);
            return 0;
        }
        offset = *(u64*)out;
        // This traversal resulted in an error
        if (offset == 0)
            break;
    }
    processMemoryClose(&session);
    free(out);
    return offset;
}

void touch(HidTouchState* state, u64 sequentialCount, u64 holdTime, bool hold, u8* token)
{
    mutexLock(&controllerMutex);
    initControllerLocked();
    mutexUnlock(&controllerMutex);
    state->delta_time = holdTime; // only the first touch needs this for whatever reason
    for (u32 i = 0; i < sequentialCount; i++)
    {
        hiddbgSetTouchScreenAutoPilotState(&state[i], 1);
        svcSleepThread(holdTime);
        if (!hold)
        {
            hiddbgSetTouchScreenAutoPilotState(NULL, 0);
            svcSleepThread(pollRate * 1e+6L);
        }

        if ((*token) == 1)
            break;
    }

    if (hold) // send finger release event
    {
        hiddbgSetTouchScreenAutoPilotState(NULL, 0);
        svcSleepThread(pollRate * 1e+6L);
    }

    hiddbgUnsetTouchScreenAutoPilotState();
}

void key(HiddbgKeyboardAutoPilotState* states, u64 sequentialCount)
{
    mutexLock(&controllerMutex);
    initControllerLocked();
    mutexUnlock(&controllerMutex);
    HiddbgKeyboardAutoPilotState tempState = { 0 };
    u32 i;
    for (i = 0; i < sequentialCount; i++)
    {
        memcpy(&tempState.keys, states[i].keys, sizeof(u64) * 4);
        tempState.modifiers = states[i].modifiers;
        hiddbgSetKeyboardAutoPilotState(&tempState);
        svcSleepThread(keyPressSleepTime * 1e+6L);

        if (i != (sequentialCount - 1))
        {
            if (memcmp(states[i].keys, states[i + 1].keys, sizeof(u64) * 4) == 0 && states[i].modifiers == states[i + 1].modifiers)
            {
                hiddbgSetKeyboardAutoPilotState(&dummyKeyboardState);
                svcSleepThread(pollRate * 1e+6L);
            }
        }
        else
        {
            hiddbgSetKeyboardAutoPilotState(&dummyKeyboardState);
            svcSleepThread(pollRate * 1e+6L);
        }
    }

    hiddbgUnsetKeyboardAutoPilotState();
}

void clickSequence(char* seq, u8* token)
{
    const char delim = ','; // used for chars and sticks
    const char startWait = 'W';
    const char startPress = '+';
    const char startRelease = '-';
    const char startLStick = '%';
    const char startRStick = '&';
    char* command = strtok(seq, &delim);
    HidNpadButton currKey = { 0 };
    u64 currentWait = 0;

    mutexLock(&controllerMutex);
    initControllerLocked();
    mutexUnlock(&controllerMutex);
    controllerSequenceActive = true;
    while (command != NULL)
    {
        if ((*token) == 1)
            break;

        if (!strncmp(command, &startLStick, 1))
        {
            // l stick
            s64 x = parseStringToSignedLong(&command[1]);
            if (x > JOYSTICK_MAX) x = JOYSTICK_MAX;
            if (x < JOYSTICK_MIN) x = JOYSTICK_MIN;
            s64 y = 0;
            command = strtok(NULL, &delim);
            if (command != NULL)
                y = parseStringToSignedLong(command);
            if (y > JOYSTICK_MAX) y = JOYSTICK_MAX;
            if (y < JOYSTICK_MIN) y = JOYSTICK_MIN;
            setStickState(JOYSTICK_LEFT, (s32)x, (s32)y);
        }
        else if (!strncmp(command, &startRStick, 1))
        {
            // r stick
            s64 x = parseStringToSignedLong(&command[1]);
            if (x > JOYSTICK_MAX) x = JOYSTICK_MAX;
            if (x < JOYSTICK_MIN) x = JOYSTICK_MIN;
            s64 y = 0;
            command = strtok(NULL, &delim);
            if (command != NULL)
                y = parseStringToSignedLong(command);
            if (y > JOYSTICK_MAX) y = JOYSTICK_MAX;
            if (y < JOYSTICK_MIN) y = JOYSTICK_MIN;
            setStickState(JOYSTICK_RIGHT, (s32)x, (s32)y);
        }
        else if (!strncmp(command, &startPress, 1))
        {
            // press
            currKey = parseStringToButton(&command[1]);
            press(currKey);
        }
        else if (!strncmp(command, &startRelease, 1))
        {
            // release
            currKey = parseStringToButton(&command[1]);
            release(currKey);
        }
        else if (!strncmp(command, &startWait, 1))
        {
            // wait
            currentWait = parseStringToInt(&command[1]);
            svcSleepThread(currentWait * 1e+6l);
        }
        else
        {
            // click
            currKey = parseStringToButton(command);
            press(currKey);
            svcSleepThread(buttonClickSleepTime * 1e+6L);
            release(currKey);
        }

        // Keep the device alive: a sequence with long waits must not be
        // released by controllerServiceIdle() half way through.
        mutexLock(&controllerMutex);
        controllerLastInputTick = armGetSystemTick();
        mutexUnlock(&controllerMutex);

        command = strtok(NULL, &delim);
    }
    controllerSequenceActive = false;
}
