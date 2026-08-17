#include "hekate_reboot.h"

#include <stdio.h>
#include <string.h>

#define HEKATE_MAGIC_OFFSET 0x118
#define HEKATE_MAGIC 0x43544349
#define HEKATE_BOOT_CONFIG_OFFSET 0x94
#define HEKATE_BOOT_ID_OFFSET 0x98
#define HEKATE_BOOT_FORCE_AUTOBOOT BIT(0)
#define HEKATE_BOOT_FROM_ID BIT(2)
#define IRAM_PAYLOAD_MAX_SIZE 0x24000
#define AMS_IWRAM_OFFSET 0x40010000

#define RTC_UPDATE0_REG 0x04
#define RTC_WRITE_UPDATE BIT(0)
#define RTC_READ_UPDATE BIT(4)
#define RTC_ALARM1_SEC_REG 0x0E
#define RTC_ALARM1_WEEKDAY_REG 0x11
#define RTC_ALARM1_YEAR_REG 0x13
#define RTC_ALARM2_WEEKDAY_REG 0x18
#define RTC_ALARM2_YEAR_REG 0x1A
#define RTC_ALARM_ENABLE BIT(7)
#define RTC_REBOOT_REASON_MAGIC 0x77

static Result i2cSendByte(I2cSession* session, u8 address, u8 value)
{
    const u8 data[2] = { address, value };
    return i2csessionSendAuto(session, data, sizeof(data), I2cTransactionOption_All);
}

static Result i2cReceiveByte(I2cSession* session, u8 address, u8* value)
{
    Result rc = i2csessionSendAuto(session, &address, sizeof(address), I2cTransactionOption_All);
    if (R_FAILED(rc)) return rc;
    return i2csessionReceiveAuto(session, value, sizeof(*value), I2cTransactionOption_All);
}

static Result stopRtcAlarms(I2cSession* session)
{
    Result rc = i2cSendByte(session, RTC_UPDATE0_REG, RTC_READ_UPDATE);
    if (R_FAILED(rc)) return rc;
    svcSleepThread(16000000ULL);
    for (unsigned i = 0; i < 14; i++) {
        u8 value = 0;
        rc = i2cReceiveByte(session, RTC_ALARM1_SEC_REG + i, &value);
        if (R_FAILED(rc)) return rc;
        rc = i2cSendByte(session, RTC_ALARM1_SEC_REG + i, value & ~RTC_ALARM_ENABLE);
        if (R_FAILED(rc)) return rc;
    }
    return i2cSendByte(session, RTC_UPDATE0_REG, RTC_WRITE_UPDATE);
}

static Result setMarikoRebootReason(unsigned mainConfigIndex)
{
    Result rc = i2cInitialize();
    if (R_FAILED(rc)) return rc;
    I2cSession session;
    rc = i2cOpenSession(&session, I2cDevice_Max77620Rtc);
    if (R_SUCCEEDED(rc)) {
        rc = stopRtcAlarms(&session);
        if (R_SUCCEEDED(rc)) {
            const u16 decoded = 1U | ((mainConfigIndex & 0xFU) << 4);
            rc = i2cSendByte(&session, RTC_ALARM1_YEAR_REG, decoded & 0x3F);
            if (R_SUCCEEDED(rc)) rc = i2cSendByte(&session, RTC_ALARM2_YEAR_REG, (decoded >> 6) & 0x3F);
            if (R_SUCCEEDED(rc)) rc = i2cSendByte(&session, RTC_ALARM1_WEEKDAY_REG, RTC_REBOOT_REASON_MAGIC);
            if (R_SUCCEEDED(rc)) rc = i2cSendByte(&session, RTC_ALARM2_WEEKDAY_REG, RTC_REBOOT_REASON_MAGIC);
            if (R_SUCCEEDED(rc)) rc = i2cSendByte(&session, RTC_UPDATE0_REG, RTC_WRITE_UPDATE);
            if (R_SUCCEEDED(rc)) svcSleepThread(16000000ULL);
        }
        i2csessionClose(&session);
    }
    i2cExit();
    return rc;
}

static bool isValidHekate(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    u32 magic = 0;
    bool valid = fseek(file, HEKATE_MAGIC_OFFSET, SEEK_SET) == 0
        && fread(&magic, 1, sizeof(magic), file) == sizeof(magic) && magic == HEKATE_MAGIC;
    fclose(file);
    return valid;
}

static Result copyEristaPayload(const char* path, const char* id)
{
    FILE* file = fopen(path, "rb");
    if (!file) return MAKERESULT(Module_Libnx, LibnxError_IoError);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > IRAM_PAYLOAD_MAX_SIZE) {
        fclose(file);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    Result rc = 0;
    for (long offset = 0; offset < size; offset += 0x1000) {
        alignas(0x1000) u8 page[0x1000] = {0};
        size_t remaining = (size_t)(size - offset);
        size_t wanted = remaining > sizeof(page) ? sizeof(page) : remaining;
        if (fread(page, 1, wanted, file) != wanted) {
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }
        if (offset == 0) {
            page[HEKATE_BOOT_CONFIG_OFFSET] = HEKATE_BOOT_FORCE_AUTOBOOT | HEKATE_BOOT_FROM_ID;
            memset(page + HEKATE_BOOT_ID_OFFSET, 0, 8);
            memcpy(page + HEKATE_BOOT_ID_OFFSET, id, strnlen(id, 7));
        }
        SecmonArgs args = {0};
        args.X[0] = 0xF0000201;
        args.X[1] = (u64)page;
        args.X[2] = AMS_IWRAM_OFFSET + offset;
        args.X[3] = wanted;
        args.X[4] = 1;
        svcCallSecureMonitor(&args);
    }
    fclose(file);
    if (R_FAILED(rc)) return rc;
    rc = splInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = splSetConfig((SplConfigItem)65001, 2);
        splExit();
    }
    return rc;
}

Result hekateRebootToId(const char* id, unsigned mainConfigIndex, const char** stage)
{
    if (!id || strlen(id) > 7 || mainConfigIndex < 1 || mainConfigIndex > 15) {
        *stage = "validateArguments";
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    const char* hekatePath = "sdmc:/bootloader/update.bin";
    if (!isValidHekate(hekatePath)) {
        *stage = "validateHekate";
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        *stage = "initializeSetSys";
        return rc;
    }
    SetSysProductModel model = SetSysProductModel_Invalid;
    rc = setsysGetProductModel(&model);
    setsysExit();
    if (R_FAILED(rc)) {
        *stage = "getProductModel";
        return rc;
    }
    if (model == SetSysProductModel_Nx || model == SetSysProductModel_Copper) {
        *stage = "copyPayload";
        return copyEristaPayload(hekatePath, id);
    }
    if (model != SetSysProductModel_Iowa && model != SetSysProductModel_Hoag
        && model != SetSysProductModel_Calcio && model != SetSysProductModel_Aula) {
        *stage = "unsupportedModel";
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }
    *stage = "setRebootReason";
    rc = setMarikoRebootReason(mainConfigIndex);
    if (R_FAILED(rc)) return rc;
    *stage = "reboot";
    rc = spsmInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = spsmShutdown(true);
        spsmExit();
    }
    return rc;
}

