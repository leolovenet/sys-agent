#pragma once

#include <switch.h>

/* Full CPU register snapshot of the most recent hardware-watchpoint hit. */
typedef struct {
    u64 x[31];      /* x0..x30 */
    u64 sp;
    u64 pc;
    u64 dataAddress; /* faulting data address (== watched address on a hit) */
    u64 threadId;
    u8 insn[12];    /* bytes at pc-8 .. pc+3, for base_reg+imm verification */
    u32 insnBytes;  /* 0 when the window could not be read */
    u8 fpStack[0x200]; /* bytes at x29 .. x29+0x200 at hit time; lets the
                        * host walk the frame-pointer chain offline */
    u32 fpStackBytes;  /* 0 when the stack window could not be read */
    u8 spStack[0x100]; /* bytes at sp .. sp+0x100 (current frame locals) */
    u32 spStackBytes;
} DebugWatchHit;

typedef struct {
    bool active;
    bool armed;
    u64 processId;
    u64 watchAddress;
    u64 watchSize;
    u32 maxHits;
    u32 hitCount;
    u32 ctxSlot;    /* discovered context-IDR breakpoint slot */
    u32 wpSlot;     /* discovered data-watchpoint register (D0 = 16) */
    u64 durationSeconds;
    u64 lastPc;
    u64 lastLr;
    u64 lastSp;
    u64 lastDataAddress;
    u64 lastThreadId;
    Result lastError;
    char stage[24];
    char hint[160];
} DebugWatchStatus;

void debugWatchInitialize(void);
bool debugWatchStart(u64 address, u64 size, u32 maxHits, u64 durationSeconds);
void debugWatchStop(void);
bool debugWatchGetStatus(DebugWatchStatus* out);
bool debugWatchGetLastHit(DebugWatchHit* out);
bool debugWatchIsActive(void);
Result debugWatchResolveMainBase(u64* outMainBase);
