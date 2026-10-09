#include <switch.h>

extern bool bControllerIsInitialised;
extern HidDeviceType controllerInitializedType;
extern u64 controllerIdleReleaseMs;
extern u64 controllerTakeoverMode;
extern HiddbgHdlsHandle controllerHandle;
extern HiddbgHdlsDeviceInfo controllerDevice;
extern HiddbgHdlsState controllerState;
extern HiddbgKeyboardAutoPilotState dummyKeyboardState;
extern Mutex controllerMutex;
extern u64 buttonClickSleepTime;
extern u64 keyPressSleepTime;
extern u64 pollRate;
extern u32 fingerDiameter;

typedef struct {
    u64 main_nso_base;
    u64 heap_base;
    u64 titleID;
    u64 titleVersion;
    u8 buildID[0x20];
} MetaData;

typedef struct {
    HidTouchState* states;
    u64 sequentialCount;
    u64 holdTime;
    bool hold;
    u8 state;
} TouchData;

typedef struct {
    HiddbgKeyboardAutoPilotState* states;
    u64 sequentialCount;
    u8 state;
} KeyData;

#define JOYSTICK_LEFT 0
#define JOYSTICK_RIGHT 1

void detachController();
void controllerRefreshIfIdle(void);
void controllerServiceIdle(void);
void controllerServiceReconnect(void);
void controllerStatusCommand(void);
void controllerDumpCommand(void);
void controllerKickCommand(const char* arg);
void controllerReconnectCommand(const char* arg, const char* addrArg);
void controllerPairedDevicesCommand(void);
u64 getTitleId(u64 pid);
u64 GetTitleVersion(u64 pid);
u64 getoutsize(NsApplicationControlData* buf);
void getBuildID(MetaData* meta, u64 pid);
MetaData getMetaData(void);
bool getIsProgramOpen(u64 id);

void poke(u64 offset, u64 size, u8* val);
/* Write, then read the range back and compare. Returns the write Result (0 when
 * the bytes were written). *readResult then holds the read-back Result and
 * *mismatch says whether a successful read-back differed from `val`, so a write
 * that landed but whose verification read failed is never reported as a write
 * failure. `readback` must hold at least `size` bytes; *readResult and *mismatch
 * are always written. */
Result pokeVerified(u64 offset, u64 size, u8* val, u8* readback, bool* mismatch,
    Result* readResult);
/* Read the range twice and accept it only when both reads agree, retrying up to
 * `attempts` times (0 means one). Returns the last read Result when the reads
 * failed, or 0 when they succeeded; *agreed then says whether two consecutive
 * reads matched. So "read returned zeros" (*agreed=1) and "read failed"
 * (nonzero Result) are distinguishable. `out` and `scratch` must each hold at
 * least `size` bytes. */
Result peekVerified(u64 offset, u64 size, u8* out, u8* scratch, u32 attempts, bool* agreed);
/* FNV-1a 32-bit fingerprint over [address, address+size), read in bounded
 * chunks with a retry per chunk. Returns the failing read Result when a chunk
 * cannot be read at all (hash is then invalid), or 0. Lets a caller tell "this
 * region changed" without pulling the bytes back. */
Result hashRegion(u64 address, u64 size, u32* hash);
void peek(u64 offset, u64 size);
void peekInfinite(u64 offset, u64 size);
void peekMulti(u64* offset, u64* size, u64 count);
void click(HidNpadButton btn);
void press(HidNpadButton btn);
void release(HidNpadButton btn);
void setStickState(int side, int dxVal, int dyVal);
void reverseArray(u8* arr, int start, int end);
u64 followMainPointer(s64* jumps, size_t count);
void touch(HidTouchState* state, u64 sequentialCount, u64 holdTime, bool hold, u8* token);
void key(HiddbgKeyboardAutoPilotState* states, u64 sequentialCount);
void clickSequence(char* seq, u8* token);
