#pragma once

#include <switch.h>

typedef enum {
    ProcessMemoryPolicyAuto = 0,
    ProcessMemoryPolicyDmnt,
    ProcessMemoryPolicyDirect
} ProcessMemoryPolicy;

typedef enum {
    ProcessMemoryBackendNone = 0,
    ProcessMemoryBackendDmnt,
    ProcessMemoryBackendDirect
} ProcessMemoryBackendKind;

typedef struct {
    u64 processId;
    u64 titleId;
    u64 mainBase;
    u64 mainSize;
    u64 heapBase;
    u64 heapSize;
    u64 aliasBase;
    u64 aliasSize;
    u64 addressSpaceBase;
    u64 addressSpaceSize;
    u8 buildId[0x20];
} ProcessMemoryMetadata;

typedef struct {
    ProcessMemoryBackendKind backend;
    u64 processId;
    Handle directHandle;
    ProcessMemoryMetadata metadata;
    bool open;
} ProcessMemorySession;

typedef struct {
    ProcessMemoryPolicy policy;
    ProcessMemoryBackendKind active;
    bool dmntAvailable;
    bool dmntAttached;
    u64 processId;
    u64 titleId;
    Result lastError;
} ProcessMemoryStatus;

void processMemoryInitialize(void);
void processMemoryExit(void);
Result processMemoryOpen(ProcessMemorySession* session, bool reportErrors);
Result processMemoryOpenBackend(ProcessMemorySession* session, ProcessMemoryBackendKind backend,
    u64 expectedProcessId, bool reportErrors);
void processMemoryClose(ProcessMemorySession* session);
Result processMemoryRead(ProcessMemorySession* session, void* output, u64 address, size_t size);
Result processMemoryWrite(ProcessMemorySession* session, const void* input, u64 address, size_t size);
Result processMemoryQuery(ProcessMemorySession* session, MemoryInfo* info, u64 address);
Result processMemoryPause(ProcessMemorySession* session);
Result processMemoryResume(ProcessMemorySession* session);
const ProcessMemoryMetadata* processMemoryGetMetadata(const ProcessMemorySession* session);
ProcessMemoryPolicy processMemoryGetPolicy(void);
bool processMemorySetPolicy(ProcessMemoryPolicy policy);
void processMemoryGetStatus(ProcessMemoryStatus* status);
bool processMemoryParsePolicy(const char* text, ProcessMemoryPolicy* policy);
const char* processMemoryPolicyName(ProcessMemoryPolicy policy);
const char* processMemoryBackendName(ProcessMemoryBackendKind backend);
