#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "search_store.h"

static void setBit(uint8_t* mask, unsigned bit)
{
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

int main(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/sys-botbase-store-%ld.dat", (long)getpid());
    remove(path);
    SearchStoreMetadata metadata = {
        .processId = 7,
        .titleId = 8,
        .start = 0x1000,
        .end = 0x4000,
        .alignment = 4,
        .width = 4,
        .generation = 2,
    };
    uint8_t values[8 * 4];
    for (unsigned index = 0; index < sizeof(values); index++)
        values[index] = (uint8_t)index;

    SearchStoreWriter writer;
    assert(searchStoreWriterOpen(&writer, path, &metadata));
    assert(searchStoreWriterAddBlock(&writer, 0x1000, 8, values, NULL));
    uint8_t sparseMask[1] = { 0 };
    setBit(sparseMask, 1);
    setBit(sparseMask, 6);
    assert(searchStoreWriterAddBlock(&writer, 0x2000, 8, values, sparseMask));
    uint8_t bitmapMask[1] = { 0x7F };
    assert(searchStoreWriterAddBlock(&writer, 0x3000, 8, values, bitmapMask));
    assert(searchStoreWriterFinish(&writer));

    SearchStoreReader reader;
    assert(searchStoreReaderOpen(&reader, path));
    assert(reader.header.candidateCount == 17);
    uint8_t decoded[sizeof(values)];
    uint8_t mask[1];
    SearchStoreBlockHeader block;
    assert(searchStoreReaderNext(&reader, &block, decoded, sizeof(decoded), mask, sizeof(mask)));
    assert(block.base == 0x1000 && block.candidateCount == 8 && mask[0] == 0xFF);
    assert(memcmp(decoded, values, sizeof(values)) == 0);
    assert(searchStoreReaderNext(&reader, &block, decoded, sizeof(decoded), mask, sizeof(mask)));
    assert(block.base == 0x2000 && block.candidateCount == 2 && mask[0] == 0x42);
    assert(searchStoreReaderNext(&reader, &block, decoded, sizeof(decoded), mask, sizeof(mask)));
    assert(block.base == 0x3000 && block.candidateCount == 7 && mask[0] == 0x7F);
    searchStoreReaderClose(&reader);

    uint64_t addresses[6], copied = 0, total = 0;
    assert(searchStoreReadAddresses(path, 7, 6, addresses, &copied, &total));
    assert(total == 17 && copied == 6);
    assert(addresses[0] == 0x101C);
    assert(addresses[1] == 0x2004);
    assert(addresses[2] == 0x2018);
    assert(addresses[3] == 0x3000);
    assert(addresses[4] == 0x3004);
    assert(addresses[5] == 0x3008);

    assert(searchStoreEstimateMaximum(0x100000, 1, 8) > 0x800000);
    uint64_t available = 0;
    assert(searchStoreHasFreeSpace("/tmp", 1, 0, &available));
    assert(available > 0);
    assert(!searchStoreHasFreeSpace("/tmp", UINT64_MAX, 1, NULL));

    FILE* corrupt = fopen(path, "r+b");
    assert(corrupt != NULL);
    assert(fseek(corrupt, (long)(sizeof(SearchStoreHeader) + sizeof(SearchStoreBlockHeader)),
        SEEK_SET) == 0);
    int byte = fgetc(corrupt);
    assert(byte != EOF);
    assert(fseek(corrupt, -1, SEEK_CUR) == 0);
    assert(fputc(byte ^ 0xFF, corrupt) != EOF);
    assert(fclose(corrupt) == 0);
    assert(searchStoreReaderOpen(&reader, path));
    assert(!searchStoreReaderNext(&reader, &block, decoded, sizeof(decoded), mask, sizeof(mask)));
    searchStoreReaderClose(&reader);

    char directory[128], child[160], sibling[160];
    snprintf(directory, sizeof(directory), "/tmp/sys-botbase-store-dir-%ld", (long)getpid());
    snprintf(child, sizeof(child), "%s/session.tmp", directory);
    snprintf(sibling, sizeof(sibling), "%s-sibling", directory);
    assert(searchStorePrepareDirectory(directory));
    FILE* temporary = fopen(child, "wb");
    assert(temporary != NULL && fclose(temporary) == 0);
    FILE* outside = fopen(sibling, "wb");
    assert(outside != NULL && fclose(outside) == 0);
    assert(searchStoreClearDirectory(directory));
    assert(access(child, F_OK) != 0);
    assert(access(sibling, F_OK) == 0);
    remove(sibling);
    rmdir(directory);

    char current[128], temporaryPath[128], backup[128];
    snprintf(current, sizeof(current), "/tmp/sys-botbase-current-%ld", (long)getpid());
    snprintf(temporaryPath, sizeof(temporaryPath), "/tmp/sys-botbase-temp-%ld", (long)getpid());
    snprintf(backup, sizeof(backup), "/tmp/sys-botbase-backup-%ld", (long)getpid());
    FILE* currentFile = fopen(current, "wb");
    assert(currentFile != NULL && fputs("old", currentFile) >= 0 && fclose(currentFile) == 0);
    assert(!searchStoreCommit(temporaryPath, current, backup, true));
    assert(access(current, F_OK) == 0 && access(backup, F_OK) != 0);
    FILE* temporaryFile = fopen(temporaryPath, "wb");
    assert(temporaryFile != NULL && fputs("new", temporaryFile) >= 0
        && fclose(temporaryFile) == 0);
    assert(searchStoreCommit(temporaryPath, current, backup, true));
    assert(access(current, F_OK) == 0 && access(temporaryPath, F_OK) != 0
        && access(backup, F_OK) != 0);
    remove(current);
    remove(path);
    puts("search_store_test: ok");
    return 0;
}
