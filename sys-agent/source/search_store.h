#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SEARCH_STORE_MAGIC 0x53425343u
#define SEARCH_STORE_VERSION 1u
#define SEARCH_STORE_BLOCK_MAGIC 0x4B4C4243u
#define SEARCH_STORE_PATH_MAX 256

typedef struct {
    uint64_t processId;
    uint64_t titleId;
    uint64_t start;
    uint64_t end;
    uint64_t regionBase;
    uint64_t regionOffset;
    uint64_t alignment;
    uint64_t generation;
    uint32_t type;
    uint32_t region;
    uint32_t backend;
    uint32_t width;
    uint8_t buildId[32];
} SearchStoreMetadata;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t headerSize;
    SearchStoreMetadata metadata;
    uint64_t candidateCount;
    uint64_t blockCount;
    uint64_t indexOffset;
    uint64_t dataBytes;
    uint32_t headerCrc;
    uint32_t reserved;
} SearchStoreHeader;

typedef struct {
    uint32_t magic;
    uint8_t representation;
    uint8_t width;
    uint16_t reserved;
    uint64_t base;
    uint64_t cumulativeCandidates;
    uint32_t slotCount;
    uint32_t candidateCount;
    uint32_t payloadSize;
    uint32_t payloadCrc;
} SearchStoreBlockHeader;

typedef struct {
    uint64_t fileOffset;
    uint64_t cumulativeCandidates;
    uint64_t base;
    uint32_t slotCount;
    uint32_t candidateCount;
} SearchStoreIndexEntry;

typedef struct {
    FILE* file;
    SearchStoreHeader header;
    char path[SEARCH_STORE_PATH_MAX];
    bool failed;
} SearchStoreWriter;

typedef struct {
    FILE* file;
    SearchStoreHeader header;
    uint64_t nextBlock;
    uint64_t nextOffset;
} SearchStoreReader;

uint32_t searchStoreCrc32(const void* data, size_t size);
bool searchStoreWriterOpen(SearchStoreWriter* writer, const char* path,
    const SearchStoreMetadata* metadata);
bool searchStoreWriterAddBlock(SearchStoreWriter* writer, uint64_t base, uint32_t slotCount,
    const uint8_t* values, const uint8_t* candidateMask);
bool searchStoreWriterFinish(SearchStoreWriter* writer);
void searchStoreWriterAbort(SearchStoreWriter* writer);

bool searchStoreReaderOpen(SearchStoreReader* reader, const char* path);
void searchStoreReaderClose(SearchStoreReader* reader);
bool searchStoreReaderNext(SearchStoreReader* reader, SearchStoreBlockHeader* block,
    uint8_t* values, size_t valuesCapacity, uint8_t* mask, size_t maskCapacity);
bool searchStoreReadAddresses(const char* path, uint64_t offset, uint64_t count,
    uint64_t* addresses, uint64_t* copied, uint64_t* total);
bool searchStoreCommit(const char* temporary, const char* current, const char* backup,
    bool currentExists);

uint64_t searchStoreEstimateMaximum(uint64_t size, uint64_t alignment, uint32_t width);
bool searchStoreHasFreeSpace(const char* root, uint64_t required, uint64_t reserve,
    uint64_t* available);
bool searchStorePrepareDirectory(const char* path);
bool searchStoreClearDirectory(const char* path);
