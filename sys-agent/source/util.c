#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <switch.h>
#include "util.h"

// taken from sys-httpd (thanks jolan!)
static const HidsysNotificationLedPattern breathingpattern = {
    .baseMiniCycleDuration = 0x8, // 100ms.
    .totalMiniCycles = 0x2,       // 3 mini cycles. Last one 12.5ms.
    .totalFullCycles = 0x2,       // 2 full cycles.
    .startIntensity = 0x2,        // 13%.
    .miniCycles = {
        // First cycle
        {
            .ledIntensity = 0xF,      // 100%.
            .transitionSteps = 0xF,   // 15 steps. Transition time 1.5s.
            .finalStepDuration = 0x0, // 12.5ms.
        },
        // Second cycle
        {
            .ledIntensity = 0x2,      // 13%.
            .transitionSteps = 0xF,   // 15 steps. Transition time 1.5s.
            .finalStepDuration = 0x0, // 12.5ms.
        },
    },
};

// beeg flash for wireless controller
static const HidsysNotificationLedPattern flashpattern = {
    .baseMiniCycleDuration = 0xF, // 200ms.
    .totalMiniCycles = 0x2,       // 3 mini cycles. Last one 12.5ms.
    .totalFullCycles = 0x2,       // 2 full cycles.
    .startIntensity = 0xF,        // 100%.
    .miniCycles = {
        // First and cloned cycle
        {
            .ledIntensity = 0xF,      // 100%.
            .transitionSteps = 0xF,   // 15 steps. Transition time 1.5s.
            .finalStepDuration = 0x0, // 12.5ms.
        },
        // clone
        {
            .ledIntensity = 0xF,      // 100%.
            .transitionSteps = 0xF,   // 15 steps. Transition time 1.5s.
            .finalStepDuration = 0x0, // 12.5ms.
        },
    },
};

int setupServerSocket()
{
    int lissock;
    int yes = 1;
    struct sockaddr_in server;
    lissock = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(lissock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(6000);

    while (bind(lissock, (struct sockaddr*)&server, sizeof(server)) < 0)
    {
        svcSleepThread(1e+9L);
    }
    listen(lissock, 3);
    return lissock;
}

u64 parseStringToInt(char* arg) {
    /* Conventions: "0x"/"0X" prefix = hexadecimal, otherwise decimal.
     * The whole string must be consumed; invalid input (e.g. bare "12AB",
     * which would previously parse as decimal 12) returns 0 instead of a
     * silently wrong partial value. */
    if (arg == NULL || arg[0] == 0)
        return 0;

    int base = 10;
    const char* digits = arg;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        base = 16;
        digits += 2;
        if (digits[0] == 0)
            return 0;
    }

    char* end = NULL;
    errno = 0;
    const unsigned long long ret = strtoull(digits, &end, base);
    if (errno != 0 || end == digits || *end != 0)
        return 0;
    return (u64)ret;
}

bool tryParseStringToInt(const char* arg, u64* value)
{
    if (arg == NULL || value == NULL || arg[0] == 0 || arg[0] == '-')
        return false;

    int base = 10;
    const char* digits = arg;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        base = 16;
        digits += 2;
        if (digits[0] == 0)
            return false;
    }

    char* end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(digits, &end, base);
    if (errno != 0 || end == digits || *end != 0)
        return false;
    *value = (u64)parsed;
    return true;
}

s64 parseStringToSignedLong(char* arg) {
    /* Signed variant used for pointer jumps.  Handles an optional sign,
     * then the same 0x/0X-hex else decimal convention, with full-string
     * validation (invalid input returns 0). */
    if (arg == NULL || arg[0] == 0)
        return 0;

    int base = 10;
    const char* signless = arg;
    if (*signless == '-' || *signless == '+')
        signless++;
    if (signless[0] == '0' && (signless[1] == 'x' || signless[1] == 'X'))
        base = 16;

    char* end = NULL;
    errno = 0;
    const long long ret = strtoll(arg, &end, base);
    if (errno != 0 || end == arg || *end != 0)
        return 0;
    return (s64)ret;
}

u8* parseStringToByteBuffer(char* arg, u64* size)
{
    if (arg == NULL || size == NULL) {
        if (size != NULL)
            *size = 0;
        return NULL;
    }

    int length = strlen(arg);

    /* Accept an optional 0x prefix.  The payload is ALWAYS hex byte pairs;
     * parsing it as decimal silently corrupts multi-byte data (e.g. a code
     * patch "50 00..." became decimal 0x32 0x00..., corrupting B and
     * crashing the game). */
    if (length > 2 && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        length -= 2;
        arg = &arg[2]; /* cut off 0x */
    }

    /* Validate every character before allocating/parsing; silently accepting
     * non-hex input produced the wrong-bytes crash class.  Empty payload and
     * odd length are also rejected (a bare "F" would otherwise mean 0x0F,
     * which is too easy to misread). */
    if (length == 0 || (length % 2) != 0) {
        *size = 0;
        return NULL;
    }
    int i = 0;
    for (i = 0; i < length; i++) {
        const char c = arg[i];
        const bool isHexDigit =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
            || (c >= 'A' && c <= 'F');
        if (!isHexDigit) {
            *size = 0;
            return NULL;
        }
    }

    char toTranslate[3] = { 0 };
    u64 bufferSize = length / 2;
    u8* buffer = malloc(bufferSize);

    for (i = 0; i < bufferSize; i++) {
        toTranslate[0] = arg[i * 2];
        toTranslate[1] = arg[(i * 2) + 1];
        buffer[i] = (u8)strtoul(toTranslate, NULL, 16);
    }
    *size = bufferSize;
    return buffer;
}

HidNpadButton parseStringToButton(char* arg)
{
    if (strcmp(arg, "A") == 0)
    {
        return HidNpadButton_A;
    }
    else if (strcmp(arg, "B") == 0)
    {
        return HidNpadButton_B;
    }
    else if (strcmp(arg, "X") == 0)
    {
        return HidNpadButton_X;
    }
    else if (strcmp(arg, "Y") == 0)
    {
        return HidNpadButton_Y;
    }
    else if (strcmp(arg, "RSTICK") == 0)
    {
        return HidNpadButton_StickR;
    }
    else if (strcmp(arg, "LSTICK") == 0)
    {
        return HidNpadButton_StickL;
    }
    else if (strcmp(arg, "L") == 0)
    {
        return HidNpadButton_L;
    }
    else if (strcmp(arg, "R") == 0)
    {
        return HidNpadButton_R;
    }
    else if (strcmp(arg, "ZL") == 0)
    {
        return HidNpadButton_ZL;
    }
    else if (strcmp(arg, "ZR") == 0)
    {
        return HidNpadButton_ZR;
    }
    else if (strcmp(arg, "PLUS") == 0)
    {
        return HidNpadButton_Plus;
    }
    else if (strcmp(arg, "MINUS") == 0)
    {
        return HidNpadButton_Minus;
    }
    else if (strcmp(arg, "DLEFT") == 0 || strcmp(arg, "DL") == 0)
    {
        return HidNpadButton_Left;
    }
    else if (strcmp(arg, "DUP") == 0 || strcmp(arg, "DU") == 0)
    {
        return HidNpadButton_Up;
    }
    else if (strcmp(arg, "DRIGHT") == 0 || strcmp(arg, "DR") == 0)
    {
        return HidNpadButton_Right;
    }
    else if (strcmp(arg, "DDOWN") == 0 || strcmp(arg, "DD") == 0)
    {
        return HidNpadButton_Down;
    }
    else if (strcmp(arg, "HOME") == 0)
    {
        return HiddbgNpadButton_Home;
    }
    else if (strcmp(arg, "CAPTURE") == 0)
    {
        return HiddbgNpadButton_Capture;
    }
    else if (strcmp(arg, "PALMA") == 0)
    {
        return HidNpadButton_Palma;
    }
    else if (strcmp(arg, "UNUSED") == 0) //Possibly useful for HOME button eaten issues
    {
        return BIT(20);
    }

    /* Unknown button names must NOT fall back to a real button (returning A
     * here would silently press A on a typo).  Return "no button". */
    return 0;
}

Result capsscCaptureForDebug(void* buffer, size_t buffer_size, u64* size) {
    struct {
        u32 a;
        u64 b;
    } in = { 0, 10000000000 };
    return serviceDispatchInOut(capsscGetServiceSession(), 1204, in, *size,
        .buffer_attrs = { SfBufferAttr_HipcMapTransferAllowsNonSecure | SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, buffer_size } },
        );
}

static void sendPatternStatic(const HidsysNotificationLedPattern* pattern, const HidNpadIdType idType)
{
    s32 total_entries;
    HidsysUniquePadId unique_pad_ids[2] = { 0 };

    Result rc = hidsysGetUniquePadsFromNpad(idType, unique_pad_ids, 2, &total_entries);
    if (R_FAILED(rc))
        return; // probably incompatible or no pads connected

    for (int i = 0; i < total_entries; i++)
        rc = hidsysSetNotificationLedPattern(pattern, unique_pad_ids[i]);
}

void flashLed()
{
    Result rc = hidsysInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);
    sendPatternStatic(&breathingpattern, HidNpadIdType_Handheld); // glow in and out x2 for docked joycons
    sendPatternStatic(&flashpattern, HidNpadIdType_No1); // big hard single glow for wireless/wired joycons or controllers
    hidsysExit();
}
