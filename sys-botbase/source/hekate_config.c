#include "hekate_config.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HEKATE_LINE_MAX 512
#define HEKATE_RTC_INDEX_MAX 15

static char* trim(char* value)
{
    while (isspace((unsigned char)*value)) value++;
    char* end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return value;
}

HekateConfigResult hekateFindMainConfigId(const char* path, const char* wantedId,
    unsigned* entryIndex)
{
    if (!path || !wantedId || !wantedId[0] || !entryIndex)
        return HekateConfigNotFound;
    FILE* file = fopen(path, "rb");
    if (!file) return HekateConfigOpenFailed;
    char line[HEKATE_LINE_MAX];
    unsigned index = 0, currentIndex = 0, foundIndex = 0;
    bool inBootEntry = false, found = false;
    while (fgets(line, sizeof(line), file)) {
        char* text = trim(line);
        if (!text[0] || text[0] == '#' || text[0] == ';' || text[0] == '{') continue;
        size_t length = strlen(text);
        if (text[0] == '[' && length >= 3 && text[length - 1] == ']') {
            text[length - 1] = '\0';
            inBootEntry = strcmp(trim(text + 1), "config") != 0;
            if (inBootEntry) currentIndex = ++index;
            continue;
        }
        if (!inBootEntry) continue;
        char* equals = strchr(text, '=');
        if (!equals) continue;
        *equals = '\0';
        if (strcmp(trim(text), "id") || strcmp(trim(equals + 1), wantedId)) continue;
        if (found) {
            fclose(file);
            return HekateConfigDuplicateId;
        }
        found = true;
        foundIndex = currentIndex;
    }
    fclose(file);
    if (!found) return HekateConfigNotFound;
    if (foundIndex == 0 || foundIndex > HEKATE_RTC_INDEX_MAX)
        return HekateConfigIndexOutOfRange;
    *entryIndex = foundIndex;
    return HekateConfigOk;
}

const char* hekateConfigResultName(HekateConfigResult result)
{
    switch (result) {
    case HekateConfigOk: return "OK";
    case HekateConfigOpenFailed: return "OPEN_FAILED";
    case HekateConfigNotFound: return "ID_NOT_FOUND";
    case HekateConfigDuplicateId: return "DUPLICATE_ID";
    case HekateConfigIndexOutOfRange: return "INDEX_OUT_OF_RANGE";
    }
    return "UNKNOWN";
}

