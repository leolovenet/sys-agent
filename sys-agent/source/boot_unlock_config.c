#include "boot_unlock_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_UNLOCK_CONFIG_FILE_LIMIT 4096

static char* trim(char* value)
{
    while (isspace((unsigned char)*value))
        value++;
    char* end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]))
        *--end = 0;
    return value;
}

static bool parseUnsigned(const char* value, uint32_t max, uint32_t* output)
{
    if (*value == '\0')
        return false;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0')
        return false;
    if (parsed > max)
        return false;
    *output = (uint32_t)parsed;
    return true;
}

void bootUnlockConfigDefaults(BootUnlockConfig* config)
{
    memset(config, 0, sizeof(*config));
    config->mode = BootUnlockModeArmed;
    config->firstDelayMs = BOOT_UNLOCK_DEFAULT_FIRST_DELAY_MS;
    config->wakeAttempts = BOOT_UNLOCK_DEFAULT_WAKE_ATTEMPTS;
    config->wakeDelayMs = BOOT_UNLOCK_DEFAULT_WAKE_DELAY_MS;
    config->verifyDelayMs = BOOT_UNLOCK_DEFAULT_VERIFY_DELAY_MS;
    config->pressGapMs = BOOT_UNLOCK_DEFAULT_PRESS_GAP_MS;
    config->burstPresses = BOOT_UNLOCK_DEFAULT_BURST_PRESSES;
    config->maxRounds = BOOT_UNLOCK_DEFAULT_MAX_ROUNDS;
    config->deadlineMs = BOOT_UNLOCK_DEFAULT_DEADLINE_MS;
}

const char* bootUnlockModeName(BootUnlockMode mode)
{
    switch (mode) {
        case BootUnlockModeOff: return "off";
        case BootUnlockModeArmed: return "armed";
        case BootUnlockModeEveryBoot: return "everyBoot";
        default: return "unknown";
    }
}

bool bootUnlockModeFromName(const char* name, BootUnlockMode* mode)
{
    if (!strcmp(name, "off")) { *mode = BootUnlockModeOff; return true; }
    if (!strcmp(name, "armed")) { *mode = BootUnlockModeArmed; return true; }
    if (!strcmp(name, "everyBoot") || !strcmp(name, "every-boot") || !strcmp(name, "always")) {
        *mode = BootUnlockModeEveryBoot;
        return true;
    }
    return false;
}

BootUnlockConfigResult bootUnlockConfigParseText(const char* text, BootUnlockConfig* config)
{
    if (!text || !config)
        return BootUnlockConfigInvalidValue;

    bootUnlockConfigDefaults(config);
    char* copy = malloc(strlen(text) + 1);
    if (!copy)
        return BootUnlockConfigInvalidValue;
    strcpy(copy, text);

    bool inSection = false;
    BootUnlockConfigResult result = BootUnlockConfigOk;
    char* save = NULL;
    for (char* line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        line = trim(line);
        if (!*line || *line == '#' || *line == ';')
            continue;
        if (*line == '[') {
            char* close = strchr(line, ']');
            if (!close) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
            *close = 0;
            inSection = !strcmp(trim(line + 1), "boot-unlock")
                     || !strcmp(trim(line + 1), "bootunlock");
            continue;
        }
        if (!inSection)
            continue;

        char* equals = strchr(line, '=');
        if (!equals) {
            result = BootUnlockConfigInvalidValue;
            break;
        }
        *equals = 0;
        char* key = trim(line);
        char* value = trim(equals + 1);
        /* Strip a trailing comment so `presses=3   # probes` reads naturally. */
        for (char* cursor = value; *cursor; cursor++) {
            if ((*cursor == '#' || *cursor == ';') &&
                (cursor == value || isspace((unsigned char)cursor[-1]))) {
                *cursor = 0;
                break;
            }
        }
        value = trim(value);

        if (!strcmp(key, "mode")) {
            if (!bootUnlockModeFromName(value, &config->mode)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "firstDelayMs")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_FIRST_DELAY_MS, &config->firstDelayMs)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "wakeDelayMs")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_WAKE_DELAY_MS, &config->wakeDelayMs)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "wakeAttempts")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_WAKE_ATTEMPTS, &config->wakeAttempts)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "verifyDelayMs")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_VERIFY_DELAY_MS, &config->verifyDelayMs)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "pressGapMs")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_PRESS_GAP_MS, &config->pressGapMs)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "burstPresses")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_BURST_PRESSES, &config->burstPresses)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "maxRounds")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_ROUNDS, &config->maxRounds)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        else if (!strcmp(key, "deadlineMs")) {
            if (!parseUnsigned(value, BOOT_UNLOCK_MAX_DEADLINE_MS, &config->deadlineMs)) {
                result = BootUnlockConfigInvalidValue;
                break;
            }
        }
        /* Anything else is a key this build does not know; ignoring it keeps an
         * older sysmodule usable with a newer config file. */
    }

    if (result == BootUnlockConfigOk) {
        /* The burst has to stay inside the lock screen's ~1 s "same button again"
         * window, and every count has to leave the worker something to do. */
        if (config->maxRounds == 0 || config->burstPresses == 0 ||
            config->pressGapMs == 0 || config->deadlineMs == 0 ||
            config->pressGapMs >= 1000)
            result = BootUnlockConfigInvalidValue;
    }

    free(copy);
    return result;
}

BootUnlockConfigResult bootUnlockConfigLoad(const char* path, BootUnlockConfig* config)
{
    bootUnlockConfigDefaults(config);

    FILE* file = fopen(path, "rb");
    if (!file)
        return errno == ENOENT ? BootUnlockConfigNotFound : BootUnlockConfigIoError;

    char buffer[BOOT_UNLOCK_CONFIG_FILE_LIMIT + 1];
    size_t size = fread(buffer, 1, BOOT_UNLOCK_CONFIG_FILE_LIMIT + 1, file);
    bool failed = ferror(file) || size > BOOT_UNLOCK_CONFIG_FILE_LIMIT;
    fclose(file);
    if (failed)
        return BootUnlockConfigIoError;
    buffer[size] = 0;
    return bootUnlockConfigParseText(buffer, config);
}

const char* bootUnlockConfigResultName(BootUnlockConfigResult result)
{
    switch (result) {
        case BootUnlockConfigOk: return "ok";
        case BootUnlockConfigNotFound: return "not_found";
        case BootUnlockConfigInvalidValue: return "invalid_value";
        case BootUnlockConfigIoError: return "io_error";
        default: return "unknown";
    }
}
