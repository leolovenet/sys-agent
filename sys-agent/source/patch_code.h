#pragma once

#include <switch.h>

/* Transactional code patch via the direct debug handle.
 *
 * Generic toolbox primitive (not game-specific): pause the target process,
 * optionally verify no thread PC is inside the patched range, optionally
 * verify the original bytes, write the patch, read it back, resume, and
 * detach -- all as one transaction.  Resume is guaranteed on every error
 * path.  Any size up to PATCH_CODE_MAX_SIZE is supported; the size is derived
 * from the payload, not hard-coded to a particular hook layout.  Cache
 * maintenance for the target is performed by the kernel's debug-write path;
 * this primitive never issues cache ops on the target address from the
 * sysmodule's own address space (that faults and panics the system).
 */

#define PATCH_CODE_MAX_SIZE 0x400 /* 1 KiB per patch, arbitrary ceiling */

typedef enum {
    PatchCodeOk = 0,
    PatchCodeBusyPcInRange,
    PatchCodeExpectedMismatch,
    PatchCodeReadbackFailed,
} PatchCodeStatus;

typedef struct {
    PatchCodeStatus status;
    bool dmntClosed;
    u32 pausedThreads;
    u64 nearestPc;      /* PC closest to [address, address+patchSize) */
    u64 pcHitThread;    /* thread id whose PC was inside the range */
    u64 pcHitValue;     /* that thread's PC */
    u64 patchSize;      /* number of bytes patched */
    u8 oldBytes[PATCH_CODE_MAX_SIZE]; /* bytes read before the write */
    u8 newBytes[PATCH_CODE_MAX_SIZE]; /* bytes read back after the write */
} PatchCodeResult;

/* Runs the full pause -> verify -> write -> readback -> resume transaction.
 * pid == 0 targets the foreground application; otherwise the given process id.
 * expected may be NULL when skipVerify is true. checkPc=false skips the thread
 * PC overlap check (only for provably cold code).
 * Returns a Result for hard failures; status inside *out classifies soft
 * rejections (PC overlap, expected mismatch, readback mismatch). */
Result patchCodeRun(u64 address, const u8* expected, u64 expectedSize,
    bool skipVerify, const u8* patch, u64 patchSize, bool checkPc, u64 pid,
    PatchCodeResult* out);
