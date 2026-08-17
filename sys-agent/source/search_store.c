#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "search_store.h"

enum {
    StoreRepresentationAll = 1,
    StoreRepresentationBitmap = 2,
    StoreRepresentationSparse = 3,
    StoreMaximumPayload = 0x50000
};

static bool bitIsSet(const uint8_t* mask, uint32_t index)
{
    return (mask[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
}

static void bitSet(uint8_t* mask, uint32_t index)
{
    mask[index >> 3] |= (uint8_t)(1u << (index & 7));
}

uint32_t searchStoreCrc32(const void* data, size_t size)
{
    const uint8_t* bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < size; index++) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static uint32_t headerCrc(const SearchStoreHeader* header)
{
    SearchStoreHeader copy = *header;
    copy.headerCrc = 0;
    return searchStoreCrc32(&copy, sizeof(copy));
}

bool searchStoreWriterOpen(SearchStoreWriter* writer, const char* path,
    const SearchStoreMetadata* metadata)
{
    memset(writer, 0, sizeof(*writer));
    if (strlen(path) >= sizeof(writer->path))
        return false;
    memcpy(writer->path, path, strlen(path) + 1);
    writer->file = fopen(path, "w+b");
    if (writer->file == NULL)
        return false;
    writer->header.magic = SEARCH_STORE_MAGIC;
    writer->header.version = SEARCH_STORE_VERSION;
    writer->header.headerSize = sizeof(SearchStoreHeader);
    writer->header.metadata = *metadata;
    if (fwrite(&writer->header, sizeof(writer->header), 1, writer->file) != 1) {
        searchStoreWriterAbort(writer);
        return false;
    }
    return true;
}

bool searchStoreWriterAddBlock(SearchStoreWriter* writer, uint64_t base, uint32_t slotCount,
    const uint8_t* values, const uint8_t* candidateMask)
{
    if (writer->file == NULL || writer->failed || slotCount == 0)
        return false;
    const uint32_t width = writer->header.metadata.width;
    const size_t maskSize = (slotCount + 7u) / 8u;
    uint32_t candidateCount = slotCount;
    if (candidateMask != NULL) {
        candidateCount = 0;
        for (uint32_t slot = 0; slot < slotCount; slot++)
            candidateCount += bitIsSet(candidateMask, slot) ? 1u : 0u;
    }
    if (candidateCount == 0)
        return true;

    const uint64_t allSize = (uint64_t)slotCount * width;
    const uint64_t bitmapSize = maskSize + allSize;
    const uint64_t sparseSize = (uint64_t)candidateCount * (sizeof(uint32_t) + width);
    uint8_t representation = StoreRepresentationAll;
    uint64_t payloadSize = allSize;
    if (candidateCount != slotCount) {
        representation = sparseSize < bitmapSize
            ? StoreRepresentationSparse : StoreRepresentationBitmap;
        payloadSize = representation == StoreRepresentationSparse ? sparseSize : bitmapSize;
    }
    if (payloadSize > UINT32_MAX || payloadSize > SIZE_MAX) {
        writer->failed = true;
        return false;
    }

    uint8_t* payload = malloc((size_t)payloadSize);
    if (payload == NULL) {
        writer->failed = true;
        return false;
    }
    if (representation == StoreRepresentationAll) {
        memcpy(payload, values, (size_t)allSize);
    }
    else if (representation == StoreRepresentationBitmap) {
        memcpy(payload, candidateMask, maskSize);
        memcpy(payload + maskSize, values, (size_t)allSize);
    }
    else {
        size_t cursor = 0;
        for (uint32_t slot = 0; slot < slotCount; slot++) {
            if (!bitIsSet(candidateMask, slot))
                continue;
            memcpy(payload + cursor, &slot, sizeof(slot));
            cursor += sizeof(slot);
            memcpy(payload + cursor, values + (size_t)slot * width, width);
            cursor += width;
        }
    }

    SearchStoreBlockHeader block = {
        .magic = SEARCH_STORE_BLOCK_MAGIC,
        .representation = representation,
        .width = width,
        .base = base,
        .cumulativeCandidates = writer->header.candidateCount,
        .slotCount = slotCount,
        .candidateCount = candidateCount,
        .payloadSize = (uint32_t)payloadSize,
        .payloadCrc = searchStoreCrc32(payload, (size_t)payloadSize),
    };
    const bool ok = fwrite(&block, sizeof(block), 1, writer->file) == 1
        && fwrite(payload, (size_t)payloadSize, 1, writer->file) == 1;
    free(payload);
    if (!ok) {
        writer->failed = true;
        return false;
    }
    writer->header.candidateCount += candidateCount;
    writer->header.blockCount++;
    writer->header.dataBytes += sizeof(block) + payloadSize;
    return true;
}

static bool writeIndex(SearchStoreWriter* writer)
{
    writer->header.indexOffset = sizeof(SearchStoreHeader) + writer->header.dataBytes;
    uint64_t offset = sizeof(SearchStoreHeader);
    for (uint64_t index = 0; index < writer->header.blockCount; index++) {
        SearchStoreBlockHeader block;
        if (fseek(writer->file, (long)offset, SEEK_SET) != 0
            || fread(&block, sizeof(block), 1, writer->file) != 1
            || block.magic != SEARCH_STORE_BLOCK_MAGIC)
            return false;
        SearchStoreIndexEntry entry = {
            .fileOffset = offset,
            .cumulativeCandidates = block.cumulativeCandidates,
            .base = block.base,
            .slotCount = block.slotCount,
            .candidateCount = block.candidateCount,
        };
        if (fseek(writer->file, 0, SEEK_END) != 0
            || fwrite(&entry, sizeof(entry), 1, writer->file) != 1)
            return false;
        offset += sizeof(block) + block.payloadSize;
    }
    return true;
}

bool searchStoreWriterFinish(SearchStoreWriter* writer)
{
    if (writer->file == NULL || writer->failed || !writeIndex(writer)) {
        searchStoreWriterAbort(writer);
        return false;
    }
    writer->header.headerCrc = headerCrc(&writer->header);
    bool ok = fseek(writer->file, 0, SEEK_SET) == 0
        && fwrite(&writer->header, sizeof(writer->header), 1, writer->file) == 1
        && fflush(writer->file) == 0
        && fsync(fileno(writer->file)) == 0
        && fclose(writer->file) == 0;
    writer->file = NULL;
    if (!ok)
        remove(writer->path);
    return ok;
}

void searchStoreWriterAbort(SearchStoreWriter* writer)
{
    if (writer->file != NULL)
        fclose(writer->file);
    writer->file = NULL;
    if (writer->path[0] != '\0')
        remove(writer->path);
}

bool searchStoreReaderOpen(SearchStoreReader* reader, const char* path)
{
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (reader->file == NULL)
        return false;
    if (fread(&reader->header, sizeof(reader->header), 1, reader->file) != 1
        || reader->header.magic != SEARCH_STORE_MAGIC
        || reader->header.version != SEARCH_STORE_VERSION
        || reader->header.headerSize != sizeof(SearchStoreHeader)
        || reader->header.headerCrc != headerCrc(&reader->header)
        || reader->header.metadata.width == 0 || reader->header.metadata.width > 8) {
        searchStoreReaderClose(reader);
        return false;
    }
    if (fseek(reader->file, 0, SEEK_END) != 0) {
        searchStoreReaderClose(reader);
        return false;
    }
    const long fileSize = ftell(reader->file);
    if (reader->header.indexOffset != sizeof(SearchStoreHeader) + reader->header.dataBytes
        || reader->header.blockCount > UINT64_MAX / sizeof(SearchStoreIndexEntry)
        || reader->header.indexOffset > UINT64_MAX
            - reader->header.blockCount * sizeof(SearchStoreIndexEntry)) {
        searchStoreReaderClose(reader);
        return false;
    }
    const uint64_t expected = reader->header.indexOffset
        + reader->header.blockCount * sizeof(SearchStoreIndexEntry);
    if (fileSize < 0 || (uint64_t)fileSize != expected) {
        searchStoreReaderClose(reader);
        return false;
    }
    reader->nextOffset = sizeof(SearchStoreHeader);
    return true;
}

void searchStoreReaderClose(SearchStoreReader* reader)
{
    if (reader->file != NULL)
        fclose(reader->file);
    reader->file = NULL;
}

bool searchStoreReaderNext(SearchStoreReader* reader, SearchStoreBlockHeader* block,
    uint8_t* values, size_t valuesCapacity, uint8_t* mask, size_t maskCapacity)
{
    if (reader->file == NULL || reader->nextBlock >= reader->header.blockCount)
        return false;
    if (fseek(reader->file, (long)reader->nextOffset, SEEK_SET) != 0
        || fread(block, sizeof(*block), 1, reader->file) != 1
        || block->magic != SEARCH_STORE_BLOCK_MAGIC
        || block->width != reader->header.metadata.width
        || block->slotCount == 0 || block->candidateCount == 0
        || block->candidateCount > block->slotCount
        || block->payloadSize == 0 || block->payloadSize > StoreMaximumPayload
        || reader->nextOffset > reader->header.indexOffset
        || sizeof(*block) + block->payloadSize
            > reader->header.indexOffset - reader->nextOffset)
        return false;
    uint8_t* payload = malloc(block->payloadSize);
    if (payload == NULL || fread(payload, block->payloadSize, 1, reader->file) != 1
        || searchStoreCrc32(payload, block->payloadSize) != block->payloadCrc) {
        free(payload);
        return false;
    }
    const size_t valueBytes = (size_t)block->slotCount * block->width;
    const size_t maskBytes = (block->slotCount + 7u) / 8u;
    if (valueBytes > valuesCapacity || maskBytes > maskCapacity) {
        free(payload);
        return false;
    }
    memset(values, 0, valueBytes);
    memset(mask, 0, maskBytes);
    if (block->representation == StoreRepresentationAll && block->payloadSize == valueBytes) {
        memcpy(values, payload, valueBytes);
        memset(mask, 0xFF, maskBytes);
        if ((block->slotCount & 7u) != 0)
            mask[maskBytes - 1] &= (uint8_t)((1u << (block->slotCount & 7u)) - 1u);
    }
    else if (block->representation == StoreRepresentationBitmap
        && block->payloadSize == maskBytes + valueBytes) {
        memcpy(mask, payload, maskBytes);
        memcpy(values, payload + maskBytes, valueBytes);
    }
    else if (block->representation == StoreRepresentationSparse
        && block->payloadSize == block->candidateCount * (sizeof(uint32_t) + block->width)) {
        size_t cursor = 0;
        uint32_t previousSlot = 0;
        for (uint32_t index = 0; index < block->candidateCount; index++) {
            uint32_t slot;
            memcpy(&slot, payload + cursor, sizeof(slot));
            cursor += sizeof(slot);
            if (slot >= block->slotCount || (index > 0 && slot <= previousSlot)) {
                free(payload);
                return false;
            }
            previousSlot = slot;
            bitSet(mask, slot);
            memcpy(values + (size_t)slot * block->width, payload + cursor, block->width);
            cursor += block->width;
        }
    }
    else {
        free(payload);
        return false;
    }
    free(payload);
    reader->nextOffset += sizeof(*block) + block->payloadSize;
    reader->nextBlock++;
    return true;
}

bool searchStoreReadAddresses(const char* path, uint64_t offset, uint64_t count,
    uint64_t* addresses, uint64_t* copied, uint64_t* total)
{
    *copied = 0;
    SearchStoreReader reader;
    if (!searchStoreReaderOpen(&reader, path))
        return false;
    *total = reader.header.candidateCount;
    if (offset >= *total || count == 0) {
        searchStoreReaderClose(&reader);
        return true;
    }
    uint64_t low = 0;
    uint64_t high = reader.header.blockCount;
    while (low < high) {
        const uint64_t middle = low + (high - low) / 2;
        SearchStoreIndexEntry entry;
        const uint64_t indexOffset = reader.header.indexOffset + middle * sizeof(entry);
        if (fseek(reader.file, (long)indexOffset, SEEK_SET) != 0
            || fread(&entry, sizeof(entry), 1, reader.file) != 1
            || entry.cumulativeCandidates > reader.header.candidateCount
            || entry.candidateCount > reader.header.candidateCount
                - entry.cumulativeCandidates) {
            searchStoreReaderClose(&reader);
            return false;
        }
        if (offset >= entry.cumulativeCandidates + entry.candidateCount)
            low = middle + 1;
        else
            high = middle;
    }
    for (uint64_t index = low;
        index < reader.header.blockCount && *copied < count; index++) {
        SearchStoreIndexEntry entry;
        const uint64_t indexOffset = reader.header.indexOffset + index * sizeof(entry);
        if (fseek(reader.file, (long)indexOffset, SEEK_SET) != 0
            || fread(&entry, sizeof(entry), 1, reader.file) != 1) {
            searchStoreReaderClose(&reader);
            return false;
        }
        if (offset >= entry.cumulativeCandidates + entry.candidateCount)
            continue;
        SearchStoreBlockHeader block;
        if (fseek(reader.file, (long)entry.fileOffset, SEEK_SET) != 0
            || fread(&block, sizeof(block), 1, reader.file) != 1
            || block.magic != SEARCH_STORE_BLOCK_MAGIC
            || block.width != reader.header.metadata.width
            || block.slotCount == 0 || block.candidateCount == 0
            || block.candidateCount > block.slotCount
            || block.payloadSize == 0 || block.payloadSize > StoreMaximumPayload
            || entry.fileOffset > reader.header.indexOffset
            || sizeof(block) + block.payloadSize
                > reader.header.indexOffset - entry.fileOffset
            || entry.cumulativeCandidates != block.cumulativeCandidates
            || entry.base != block.base || entry.slotCount != block.slotCount
            || entry.candidateCount != block.candidateCount
            || entry.cumulativeCandidates > reader.header.candidateCount
            || entry.candidateCount > reader.header.candidateCount
                - entry.cumulativeCandidates) {
            searchStoreReaderClose(&reader);
            return false;
        }
        uint8_t* payload = malloc(block.payloadSize);
        if (payload == NULL || fread(payload, block.payloadSize, 1, reader.file) != 1
            || searchStoreCrc32(payload, block.payloadSize) != block.payloadCrc) {
            free(payload);
            searchStoreReaderClose(&reader);
            return false;
        }
        uint64_t ordinal = block.cumulativeCandidates;
        if (block.representation == StoreRepresentationSparse) {
            size_t cursor = 0;
            uint32_t previousSlot = 0;
            for (uint32_t candidate = 0; candidate < block.candidateCount; candidate++) {
                uint32_t slot;
                memcpy(&slot, payload + cursor, sizeof(slot));
                cursor += sizeof(slot) + block.width;
                if (slot >= block.slotCount || (candidate > 0 && slot <= previousSlot)) {
                    free(payload);
                    searchStoreReaderClose(&reader);
                    return false;
                }
                previousSlot = slot;
                if (ordinal++ >= offset && *copied < count)
                    addresses[(*copied)++] = block.base + (uint64_t)slot * reader.header.metadata.alignment;
            }
        }
        else {
            const uint8_t* mask = block.representation == StoreRepresentationBitmap ? payload : NULL;
            for (uint32_t slot = 0; slot < block.slotCount && *copied < count; slot++) {
                if (mask != NULL && !bitIsSet(mask, slot))
                    continue;
                if (ordinal++ >= offset)
                    addresses[(*copied)++] = block.base + (uint64_t)slot * reader.header.metadata.alignment;
            }
        }
        free(payload);
    }
    searchStoreReaderClose(&reader);
    return true;
}

bool searchStoreCommit(const char* temporary, const char* current, const char* backup,
    bool currentExists)
{
    remove(backup);
    if (currentExists && rename(current, backup) != 0)
        return false;
    if (rename(temporary, current) != 0) {
        if (currentExists)
            rename(backup, current);
        return false;
    }
    if (currentExists)
        remove(backup);
    return true;
}

uint64_t searchStoreEstimateMaximum(uint64_t size, uint64_t alignment, uint32_t width)
{
    if (alignment == 0 || width == 0)
        return UINT64_MAX;
    const uint64_t slots = size / alignment + 1u;
    if (slots > (UINT64_MAX - sizeof(SearchStoreHeader)) / width)
        return UINT64_MAX;
    uint64_t maximumSpan = (0x40000u / width) * alignment;
    if (maximumSpan > 0x40000u)
        maximumSpan = 0x40000u;
    const uint64_t blocks = size / maximumSpan + 2u;
    const uint64_t perBlock = sizeof(SearchStoreBlockHeader) + sizeof(SearchStoreIndexEntry);
    if (blocks > UINT64_MAX / perBlock)
        return UINT64_MAX;
    const uint64_t overhead = blocks * perBlock;
    const uint64_t bitmap = (slots + 7u) / 8u;
    if (slots * width > UINT64_MAX - sizeof(SearchStoreHeader) - overhead - bitmap)
        return UINT64_MAX;
    return sizeof(SearchStoreHeader) + overhead + bitmap + slots * width;
}

bool searchStoreHasFreeSpace(const char* root, uint64_t required, uint64_t reserve,
    uint64_t* available)
{
    struct statvfs stats;
    if (statvfs(root, &stats) != 0)
        return false;
    const uint64_t freeBytes = (uint64_t)stats.f_bavail * stats.f_frsize;
    if (available != NULL)
        *available = freeBytes;
    return required <= UINT64_MAX - reserve && freeBytes >= required + reserve;
}

bool searchStorePrepareDirectory(const char* path)
{
    char current[SEARCH_STORE_PATH_MAX];
    if (strlen(path) >= sizeof(current))
        return false;
    memcpy(current, path, strlen(path) + 1);
    char* start = current + 1;
    char* device = strstr(current, ":/");
    if (device != NULL)
        start = device + 2;
    for (char* cursor = start; *cursor != '\0'; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(current, 0777) != 0 && errno != EEXIST)
            return false;
        *cursor = '/';
    }
    return mkdir(current, 0777) == 0 || errno == EEXIST;
}

bool searchStoreClearDirectory(const char* path)
{
    DIR* directory = opendir(path);
    if (directory == NULL)
        return errno == ENOENT;
    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[SEARCH_STORE_PATH_MAX];
        const int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child) || remove(child) != 0)
            ok = false;
    }
    closedir(directory);
    return ok;
}
