#pragma once

#include <switch.h>

/* Per-command output bounds. */
#define DEBUG_PROCESS_MAX_MAPS 0x100
#define DEBUG_PROCESS_MAX_THREADS 0x100
#define DEBUG_PROCESS_MAX_MODULES 2

/*
 * Generic process-debug primitives used by the `debug` command group.
 *
 * These helpers deliberately know nothing about games or specific system
 * services: any PID that the kernel lets us debug can be inspected.  Attach
 * uses the same dmnt ownership rules as the rest of sys-agent (only the
 * foreground application can be owned by dmnt:cht).
 */

/* Attach a debug handle.  pid must be non-zero; callers resolve the default
 * application PID before calling.  dmntClosed reports whether an existing
 * dmnt:cht handle for the foreground application had to be closed. */
Result debugProcessOpen(u64 pid, Handle* out, bool* dmntClosed);

/* Drain the initial CreateProcess/CreateThread/DebuggerAttached events so a
 * later svcContinueDebugEvent cannot fail on a non-empty event queue. */
Result debugProcessDrain(Handle debug);

/* Stop every thread and consume the break event. */
Result debugProcessPause(Handle debug);

/* Resume every thread (caller must have drained the event queue). */
Result debugProcessResume(Handle debug);

/* Close the debug handle (also resumes a stopped target). */
void debugProcessClose(Handle debug);

/* Print a compact map of mapped regions of the target process.  `limit`
 * bounds the number of entries printed (1..256). */
void debugProcessPrintMaps(u64 pid, u32 limit);

/* Pause the target, dump one line per thread with PC/LR/SP and scheduler
 * state, then resume. */
void debugProcessPrintThreads(u64 pid);

/* Print module base/size/Build ID information from ldr:dmnt.  This stack is
 * only known to accept two modules per request; larger counts close the
 * service session. */
void debugProcessPrintModules(u64 pid, u32 limit);

/* Resolve the main NSO base for any process through ldr:dmnt. */
Result debugProcessResolveMainBase(u64 pid, u64* outMainBase);

/* Transactionally attach, read up to `size` bytes, then release the process. */
Result debugProcessReadMemory(u64 pid, u64 address, void* out, u32 size);
