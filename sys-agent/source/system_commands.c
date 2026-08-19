#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <switch/runtime/devices/fs_dev.h>

#include "commands.h"
#include "hekate_config.h"
#include "hekate_reboot.h"
#include "process_memory.h"
#include "system_commands.h"
#include "util.h"

#define PROCESS_PAGE_MAX 64

static void printHex(const void* data, size_t size)
{
    const u8* bytes = data;
    for (size_t i = 0; i < size; i++)
        printf("%02X", bytes[i]);
}

static size_t boundedLength(const void* data, size_t size)
{
    return strnlen((const char*)data, size);
}

static void printServiceError(const char* service, Result rc)
{
    printf("ERR code=SERVICE_UNAVAILABLE service=%s result=0x%X\n", service, rc);
}

static void printCommandError(const char* stage, Result rc)
{
    printf("ERR code=COMMAND_FAILED stage=%s result=0x%X\n", stage, rc);
}

static void printIpv4(u32 address)
{
    const u8* bytes = (const u8*)&address;
    printf("%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
}

static void printIpv4Bytes(const NifmIpV4Address* address)
{
    printf("%u.%u.%u.%u", address->addr[0], address->addr[1], address->addr[2], address->addr[3]);
}

static void systemInfo(void)
{
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        printServiceError("set:sys", rc);
        return;
    }

    SetSysFirmwareVersion firmware = {0};
    SetSysFirmwareVersionDigest digest = {0};
    SetSysProductModel model = SetSysProductModel_Invalid;
    SetSysSerialNumber serial = {0};
    SetSysDeviceNickName nickname = {0};
    Result firmwareRc = setsysGetFirmwareVersion(&firmware);
    Result digestRc = setsysGetFirmwareVersionDigest(&digest);
    Result modelRc = setsysGetProductModel(&model);
    Result serialRc = setsysGetSerialNumber(&serial);
    Result nicknameRc = setsysGetDeviceNickname(&nickname);
    setsysExit();

    Result rcSet = setInitialize();
    u64 language = 0;
    SetRegion region = 0;
    Result languageRc = rcSet;
    Result regionRc = rcSet;
    if (R_SUCCEEDED(rcSet)) {
        languageRc = setGetSystemLanguage(&language);
        regionRc = setGetRegionCode(&region);
        setExit();
    }

    printf("OK firmware=");
    if (R_SUCCEEDED(firmwareRc)) printHex(firmware.display_version, boundedLength(firmware.display_version, sizeof(firmware.display_version))); else printf("NA");
    printf(" firmwareHash=");
    if (R_SUCCEEDED(firmwareRc)) printHex(firmware.version_hash, boundedLength(firmware.version_hash, sizeof(firmware.version_hash))); else printf("NA");
    printf(" platform=");
    if (R_SUCCEEDED(firmwareRc)) printHex(firmware.platform, boundedLength(firmware.platform, sizeof(firmware.platform))); else printf("NA");
    printf(" digest=");
    if (R_SUCCEEDED(digestRc)) printHex(digest.digest, boundedLength(digest.digest, sizeof(digest.digest))); else printf("NA");
    printf(" model=");
    if (R_SUCCEEDED(modelRc)) printf("%u", model); else printf("NA");
    printf(" region=");
    if (R_SUCCEEDED(regionRc)) printf("%u", region); else printf("NA");
    printf(" language=");
    if (R_SUCCEEDED(languageRc)) printf("%016lX", language); else printf("NA");
    printf(" nicknameLen=");
    if (R_SUCCEEDED(nicknameRc)) printf("%zu nickname=", boundedLength(nickname.nickname, sizeof(nickname.nickname))); else printf("NA nickname=");
    if (R_SUCCEEDED(nicknameRc)) printHex(nickname.nickname, boundedLength(nickname.nickname, sizeof(nickname.nickname))); else printf("NA");
    printf(" serialLen=");
    if (R_SUCCEEDED(serialRc)) printf("%zu serial=", boundedLength(serial.number, sizeof(serial.number))); else printf("NA serial=");
    if (R_SUCCEEDED(serialRc)) printHex(serial.number, boundedLength(serial.number, sizeof(serial.number))); else printf("NA");
    printf(" errors=firmware:0x%X,digest:0x%X,model:0x%X,region:0x%X,language:0x%X,nickname:0x%X,serial:0x%X\n",
        firmwareRc, digestRc, modelRc, regionRc, languageRc, nicknameRc, serialRc);
}

static void systemTimeCommand(void)
{
    Result rc = timeInitialize();
    if (R_FAILED(rc)) {
        printServiceError("time:s", rc);
        return;
    }
    u64 timestamp = 0;
    TimeLocationName location = {0};
    Result timestampRc = timeGetCurrentTime(TimeType_Default, &timestamp);
    Result locationRc = timeGetDeviceLocationName(&location);
    timeExit();
    u64 uptimeMs = armGetSystemTick() * 1000 / armGetSystemTickFreq();

    printf("OK unix=");
    if (R_SUCCEEDED(timestampRc)) printf("%lu", timestamp); else printf("NA");
    printf(" timezoneLen=");
    if (R_SUCCEEDED(locationRc)) printf("%zu timezone=", boundedLength(location.name, sizeof(location.name))); else printf("NA timezone=");
    if (R_SUCCEEDED(locationRc)) printHex(location.name, boundedLength(location.name, sizeof(location.name))); else printf("NA");
    printf(" uptimeMs=%lu errors=unix:0x%X,timezone:0x%X\n", uptimeMs, timestampRc, locationRc);
}

static void powerStatus(void)
{
    Result rc = psmInitialize();
    if (R_FAILED(rc)) {
        printServiceError("psm", rc);
        return;
    }
    u32 percentage = 0;
    double raw = 0, age = 0;
    PsmChargerType charger = 0;
    PsmBatteryVoltageState voltage = 0;
    bool charging = false, enough = false;
    PsmBatteryChargeInfoFields info = {0};
    Result percentageRc = psmGetBatteryChargePercentage(&percentage);
    Result rawRc = psmGetRawBatteryChargePercentage(&raw);
    Result chargerRc = psmGetChargerType(&charger);
    Result chargingRc = psmIsBatteryChargingEnabled(&charging);
    Result enoughRc = psmIsEnoughPowerSupplied(&enough);
    Result voltageRc = psmGetBatteryVoltageState(&voltage);
    Result ageRc = psmGetBatteryAgePercentage(&age);
    Result infoRc = psmGetBatteryChargeInfoFields(&info);
    psmExit();

    printf("OK percent="); if (R_SUCCEEDED(percentageRc)) printf("%u", percentage); else printf("NA");
    printf(" rawPercent="); if (R_SUCCEEDED(rawRc)) printf("%.3f", raw); else printf("NA");
    printf(" charger="); if (R_SUCCEEDED(chargerRc)) printf("%u", charger); else printf("NA");
    printf(" chargingEnabled="); if (R_SUCCEEDED(chargingRc)) printf("%d", charging); else printf("NA");
    printf(" enoughPower="); if (R_SUCCEEDED(enoughRc)) printf("%d", enough); else printf("NA");
    printf(" voltageState="); if (R_SUCCEEDED(voltageRc)) printf("%u", voltage); else printf("NA");
    printf(" agePercent="); if (R_SUCCEEDED(ageRc)) printf("%.3f", age); else printf("NA");
    if (R_SUCCEEDED(infoRc))
        printf(" temperatureMilliC=%u batteryMilliV=%u inputCurrentLimitMa=%u fastChargeCurrentLimitMa=%u fastCharging=%d",
            info.temperature_celcius, info.battery_charge_milli_voltage, info.input_current_limit,
            info.fast_charge_current_limit, info.fast_battery_charging);
    else
        printf(" temperatureMilliC=NA batteryMilliV=NA inputCurrentLimitMa=NA fastChargeCurrentLimitMa=NA fastCharging=NA");
    printf(" errors=percent:0x%X,raw:0x%X,charger:0x%X,charging:0x%X,enough:0x%X,voltage:0x%X,age:0x%X,info:0x%X\n",
        percentageRc, rawRc, chargerRc, chargingRc, enoughRc, voltageRc, ageRc, infoRc);
}

static void storageStatus(void)
{
    FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd) {
        printf("OK mounted=0 total=NA free=NA used=NA errors=space:NA\n");
        return;
    }
    s64 total = 0, free = 0;
    Result totalRc = fsFsGetTotalSpace(sd, "/", &total);
    Result freeRc = fsFsGetFreeSpace(sd, "/", &free);
    printf("OK mounted=1 total="); if (R_SUCCEEDED(totalRc)) printf("%ld", total); else printf("NA");
    printf(" free="); if (R_SUCCEEDED(freeRc)) printf("%ld", free); else printf("NA");
    printf(" used="); if (R_SUCCEEDED(totalRc) && R_SUCCEEDED(freeRc)) printf("%ld", total - free); else printf("NA");
    printf(" errors=total:0x%X,free:0x%X\n", totalRc, freeRc);
}

static Result initializeNifm(void)
{
    return nifmInitialize(NifmServiceType_System);
}

static void networkStatus(void)
{
    Result rc = initializeNifm();
    if (R_FAILED(rc)) {
        printServiceError("nifm:s", rc);
        return;
    }
    bool wireless = false, ethernet = false;
    NifmInternetConnectionType type = 0;
    NifmInternetConnectionStatus status = 0;
    u32 strength = 0, ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    Result wirelessRc = nifmIsWirelessCommunicationEnabled(&wireless);
    Result ethernetRc = nifmIsEthernetCommunicationEnabled(&ethernet);
    Result statusRc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    Result ipRc = nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &dns1, &dns2);
    nifmExit();
    printf("OK wireless="); if (R_SUCCEEDED(wirelessRc)) printf("%d", wireless); else printf("NA");
    printf(" ethernet="); if (R_SUCCEEDED(ethernetRc)) printf("%d", ethernet); else printf("NA");
    printf(" type="); if (R_SUCCEEDED(statusRc)) printf("%u", type); else printf("NA");
    printf(" status="); if (R_SUCCEEDED(statusRc)) printf("%u", status); else printf("NA");
    printf(" strength="); if (R_SUCCEEDED(statusRc)) printf("%u", strength); else printf("NA");
    printf(" ip="); if (R_SUCCEEDED(ipRc)) printIpv4(ip); else printf("NA");
    printf(" mask="); if (R_SUCCEEDED(ipRc)) printIpv4(mask); else printf("NA");
    printf(" gateway="); if (R_SUCCEEDED(ipRc)) printIpv4(gateway); else printf("NA");
    printf(" dns1="); if (R_SUCCEEDED(ipRc)) printIpv4(dns1); else printf("NA");
    printf(" dns2="); if (R_SUCCEEDED(ipRc)) printIpv4(dns2); else printf("NA");
    printf(" errors=wireless:0x%X,ethernet:0x%X,status:0x%X,ip:0x%X\n", wirelessRc, ethernetRc, statusRc, ipRc);
}

static void networkProfile(void)
{
    Result rc = initializeNifm();
    if (R_FAILED(rc)) {
        printServiceError("nifm:s", rc);
        return;
    }
    NifmNetworkProfileData profile = {0};
    rc = nifmGetCurrentNetworkProfile(&profile);
    nifmExit();
    if (R_FAILED(rc)) {
        printCommandError("getNetworkProfile", rc);
        return;
    }
    size_t networkNameLength = boundedLength(profile.network_name, sizeof(profile.network_name));
    size_t ssidLength = profile.wireless_setting_data.ssid_len;
    if (ssidLength > sizeof(profile.wireless_setting_data.ssid)) ssidLength = sizeof(profile.wireless_setting_data.ssid);
    size_t passphraseLength = boundedLength(profile.wireless_setting_data.passphrase, sizeof(profile.wireless_setting_data.passphrase));
    printf("OK uuid="); printHex(profile.uuid.uuid, sizeof(profile.uuid.uuid));
    printf(" networkNameLen=%zu networkName=", networkNameLength); printHex(profile.network_name, networkNameLength);
    printf(" ssidLen=%zu ssid=", ssidLength); printHex(profile.wireless_setting_data.ssid, ssidLength);
    printf(" passphraseLen=%zu passphrase=", passphraseLength); printHex(profile.wireless_setting_data.passphrase, passphraseLength);
    printf(" ipAutomatic=%u ip=", profile.ip_setting_data.ip_address_setting.is_automatic);
    printIpv4Bytes(&profile.ip_setting_data.ip_address_setting.current_addr);
    printf(" mask="); printIpv4Bytes(&profile.ip_setting_data.ip_address_setting.subnet_mask);
    printf(" gateway="); printIpv4Bytes(&profile.ip_setting_data.ip_address_setting.gateway);
    printf(" dnsAutomatic=%u dns1=", profile.ip_setting_data.dns_setting.is_automatic);
    printIpv4Bytes(&profile.ip_setting_data.dns_setting.primary_dns_server);
    printf(" dns2="); printIpv4Bytes(&profile.ip_setting_data.dns_setting.secondary_dns_server);
    printf(" mtu=%u\n", profile.ip_setting_data.mtu);
}

static void accountStatus(void)
{
    Result rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) {
        printServiceError("acc:u1", rc);
        return;
    }
    AccountUid uid = {0};
    rc = accountGetLastOpenedUser(&uid);
    if (R_FAILED(rc)) {
        accountExit();
        printCommandError("getLastOpenedUser", rc);
        return;
    }
    AccountProfile profile;
    AccountProfileBase base = {0};
    rc = accountGetProfile(&profile, uid);
    if (R_SUCCEEDED(rc)) {
        rc = accountProfileGet(&profile, NULL, &base);
        accountProfileClose(&profile);
    }
    accountExit();
    if (R_FAILED(rc)) {
        printCommandError("getAccountProfile", rc);
        return;
    }
    size_t nicknameLength = boundedLength(base.nickname, sizeof(base.nickname));
    printf("OK uid0=%016lX uid1=%016lX nicknameLen=%zu nickname=", uid.uid[0], uid.uid[1], nicknameLength);
    printHex(base.nickname, nicknameLength);
    printf("\n");
}

static bool getApplicationIdentity(u64* pid, u64* titleId)
{
    *pid = 0;
    *titleId = 0;
    Result rc = pmdmntGetApplicationProcessId(pid);
    if (R_FAILED(rc) || *pid == 0) return false;
    rc = pminfoGetProgramId(titleId, *pid);
    return R_SUCCEEDED(rc) && *titleId != 0;
}

static void applicationStatus(void)
{
    u64 pid, titleId;
    if (!getApplicationIdentity(&pid, &titleId)) {
        printf("OK running=0\n");
        return;
    }
    ProcessMemorySession memory;
    Result memoryRc = processMemoryOpen(&memory, false);
    u64 mainBase = 0, heapBase = 0;
    u8 buildId[0x20] = {0};
    if (R_SUCCEEDED(memoryRc)) {
        const ProcessMemoryMetadata* metadata = processMemoryGetMetadata(&memory);
        mainBase = metadata->mainBase;
        heapBase = metadata->heapBase;
        memcpy(buildId, metadata->buildId, sizeof(buildId));
        processMemoryClose(&memory);
    }

    Result rc = nsInitialize();
    bool nsActive = R_SUCCEEDED(rc);
    NsApplicationControlData* data = NULL;
    u64 outSize = 0;
    NacpLanguageEntry* language = NULL;
    u64 version = 0;
    if (R_SUCCEEDED(rc)) {
        NsApplicationContentMetaStatus statuses[16] = {0};
        s32 count = 0;
        Result versionRc = nsListApplicationContentMetaStatus(titleId, 0, statuses,
            sizeof(statuses) / sizeof(statuses[0]), &count);
        if (R_SUCCEEDED(versionRc)) {
            for (s32 i = 0; i < count; i++)
                if (statuses[i].version > version) version = statuses[i].version;
            version /= 0x10000;
        }
        data = malloc(sizeof(*data));
        if (data)
            rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId, data, sizeof(*data), &outSize);
        else
            rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    if (R_SUCCEEDED(rc) && outSize >= sizeof(data->nacp))
        nacpGetLanguageEntry(&data->nacp, &language);
    printf("OK running=1 pid=%016lX titleId=%016lX version=%lu main=", pid, titleId, version);
    if (R_SUCCEEDED(memoryRc)) printf("%016lX", mainBase); else printf("NA");
    printf(" heap="); if (R_SUCCEEDED(memoryRc)) printf("%016lX", heapBase); else printf("NA");
    printf(" buildId="); if (R_SUCCEEDED(memoryRc)) printHex(buildId, sizeof(buildId)); else printf("NA");
    printf(" nameLen=");
    if (language) {
        size_t length = boundedLength(language->name, sizeof(language->name));
        printf("%zu name=", length);
        printHex(language->name, length);
    } else {
        printf("NA name=NA");
    }
    printf(" errors=memory:0x%X,name:0x%X\n", memoryRc, rc);
    free(data);
    if (nsActive) nsExit();
}

static void processList(u64 offset, u64 count)
{
    u64 pids[0x100] = {0};
    s32 total = 0;
    Result rc = svcGetProcessList(&total, pids, sizeof(pids) / sizeof(pids[0]));
    if (R_FAILED(rc)) {
        printCommandError("getProcessList", rc);
        return;
    }
    const u64 captured = total < (s32)(sizeof(pids) / sizeof(pids[0]))
        ? (u64)total : sizeof(pids) / sizeof(pids[0]);
    if (offset > captured) offset = captured;
    u64 available = captured - offset;
    if (count > available) count = available;
    printf("OK total=%d captured=%lu offset=%lu count=%lu processes=", total, captured, offset, count);
    for (u64 i = 0; i < count; i++) {
        u64 titleId = 0;
        Result titleRc = pminfoGetProgramId(&titleId, pids[offset + i]);
        printf("%s%016lX:%s", i ? "," : "", pids[offset + i], R_SUCCEEDED(titleRc) ? "" : "NA");
        if (R_SUCCEEDED(titleRc)) printf("%016lX", titleId);
    }
    printf("\n");
}

static void powerAction(bool reboot)
{
    Result rc = bpcInitialize();
    if (R_FAILED(rc)) {
        printServiceError("bpc", rc);
        return;
    }
    rc = reboot ? bpcRebootSystem() : bpcShutdownSystem();
    bpcExit();
    if (R_FAILED(rc)) {
        printCommandError(reboot ? "reboot" : "shutdown", rc);
        return;
    }
    printf("OK action=%s\n", reboot ? "rebooting" : "shuttingDown");
    fflush(stdout);
}

static void rebootEmuMmc(void)
{
    static const char* id = "Atm-Emu";
    unsigned entryIndex = 0;
    HekateConfigResult config = hekateFindMainConfigId(
        "sdmc:/bootloader/hekate_ipl.ini", id, &entryIndex);
    if (config != HekateConfigOk) {
        printf("ERR code=HEKATE_CONFIG_ERROR detail=%s id=%s\n",
            hekateConfigResultName(config), id);
        return;
    }
    const char* stage = "unknown";
    Result rc = hekateRebootToId(id, entryIndex, &stage);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=%s result=0x%X\n", stage, rc);
        return;
    }
    printf("OK action=rebooting target=emuMMC id=%s entry=%u\n", id, entryIndex);
    fflush(stdout);
}

static void networkSet(bool enabled)
{
    Result rc = initializeNifm();
    if (R_FAILED(rc)) {
        printServiceError("nifm:s", rc);
        return;
    }
    rc = nifmSetWirelessCommunicationEnabled(enabled);
    nifmExit();
    if (R_FAILED(rc)) {
        printCommandError("setWireless", rc);
        return;
    }
    printf("OK wireless=%d\n", enabled);
    fflush(stdout);
}

static void lockScreenStatus(void)
{
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        printServiceError("set:sys", rc);
        return;
    }
    bool enabled = false;
    rc = setsysGetLockScreenFlag(&enabled);
    setsysExit();
    if (R_FAILED(rc)) {
        printCommandError("getLockScreen", rc);
        return;
    }
    printf("OK lockScreen=%d\n", enabled);
}

static void lockScreenSet(bool enabled)
{
    Result rc = setsysInitialize();
    if (R_FAILED(rc)) {
        printServiceError("set:sys", rc);
        return;
    }
    rc = setsysSetLockScreenFlag(enabled);
    setsysExit();
    if (R_FAILED(rc)) {
        printCommandError("setLockScreen", rc);
        return;
    }
    printf("OK lockScreen=%d\n", enabled);
}

static const char* audioTargetName(AudioTarget target)
{
    switch (target) {
        case AudioTarget_Speaker: return "speaker";
        case AudioTarget_Headphone: return "headphone";
        case AudioTarget_Tv: return "tv";
        case AudioTarget_UsbOutputDevice: return "usb";
        case AudioTarget_Bluetooth: return "bluetooth";
        default: return "invalid";
    }
}

static bool audioResolveTarget(AudioTarget* target)
{
    Result rc = audctlGetActiveOutputTarget(target);
    if (R_SUCCEEDED(rc) && *target != AudioTarget_Invalid)
        return true;
    rc = audctlGetDefaultTarget(target);
    return R_SUCCEEDED(rc) && *target != AudioTarget_Invalid;
}

static void audioVolumeCommand(bool set, u64 value)
{
    Result rc = audctlInitialize();
    if (R_FAILED(rc)) {
        printServiceError("aud:ctl", rc);
        return;
    }
    if (set) {
        rc = audctlSetSystemOutputMasterVolume((float)value / 100.0f);
        if (R_FAILED(rc)) {
            audctlExit();
            printCommandError("setMasterVolume", rc);
            return;
        }
    }
    float volume = 0.0f;
    rc = audctlGetSystemOutputMasterVolume(&volume);
    audctlExit();
    if (R_FAILED(rc)) {
        printCommandError("getMasterVolume", rc);
        return;
    }
    u64 rounded = (u64)(volume * 100.0f + 0.5f);
    if (rounded > 100)
        rounded = 100;
    printf("OK volume=%lu\n", rounded);
    fflush(stdout);
}

static void audioMuteCommand(const char* state)
{
    Result rc = audctlInitialize();
    if (R_FAILED(rc)) {
        printServiceError("aud:ctl", rc);
        return;
    }
    AudioTarget target = AudioTarget_Invalid;
    if (!audioResolveTarget(&target)) {
        audctlExit();
        printf("ERR code=NO_AUDIO_TARGET\n");
        return;
    }
    if (state) {
        rc = audctlSetTargetMute(target, !strcmp(state, "enabled"));
        if (R_FAILED(rc)) {
            audctlExit();
            printCommandError("setTargetMute", rc);
            return;
        }
    }
    bool mute = false;
    rc = audctlIsTargetMute(&mute, target);
    audctlExit();
    if (R_FAILED(rc)) {
        printCommandError("isTargetMute", rc);
        return;
    }
    printf("OK mute=%d target=%s\n", mute ? 1 : 0, audioTargetName(target));
    fflush(stdout);
}

static void applicationTerminate(void)
{
    u64 pid, titleId;
    if (!getApplicationIdentity(&pid, &titleId)) {
        printf("ERR code=NO_APPLICATION\n");
        return;
    }
    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        printServiceError("pm:shell", rc);
        return;
    }
    rc = pmshellTerminateProgram(titleId);
    pmshellExit();
    if (R_FAILED(rc)) {
        printCommandError("terminateApplication", rc);
        return;
    }
    printf("OK action=terminated titleId=%016lX\n", titleId);
}

static bool parseTitleId(const char* text, u64* titleId)
{
    if (text == NULL || titleId == NULL)
        return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    size_t length = strlen(text);
    if (length == 0 || length > 16)
        return false;
    u64 value = 0;
    for (size_t i = 0; i < length; i++) {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F')
            digit = (unsigned)(c - 'A') + 10;
        else
            return false;
        value = (value << 4) | digit;
    }
    if (value == 0)
        return false;
    *titleId = value;
    return true;
}

static void gameLaunchHeadlessCommand(u64 titleId)
{
    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        printServiceError("pm:shell", rc);
        return;
    }
    /* pmshellLaunchProgram needs the real install storage: with
     * NcmStorageId_None the loader/fsp cannot resolve the title's NCA and
     * pm:shell fails with an lr path-not-found result (e.g. 0xA5800A08).
     * Nintendo always passes the storage the title is installed on, so try
     * the common storages in order and use the first one pm accepts. */
    enum { LaunchStorageCount = 4 };
    static const NcmStorageId launchStorageOrder[LaunchStorageCount] = {
        NcmStorageId_SdCard, NcmStorageId_BuiltInUser,
        NcmStorageId_GameCard, NcmStorageId_None,
    };
    static const char* storageNames[LaunchStorageCount] = {
        "SdCard", "BuiltInUser", "GameCard", "None",
    };
    const char* usedStorageName = "None";
    Result firstRc = 0;
    Result attemptRcs[LaunchStorageCount] = {0};
    u64 pid = 0;
    for (size_t i = 0; i < LaunchStorageCount; i++) {
        NcmProgramLocation location = { .program_id = titleId, .storageID = launchStorageOrder[i] };
        rc = pmshellLaunchProgram(0, &location, &pid);
        attemptRcs[i] = rc;
        if (R_SUCCEEDED(rc)) {
            usedStorageName = storageNames[i];
            break;
        }
        if (firstRc == 0)
            firstRc = rc;
    }
    pmshellExit();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=launchProgram result=0x%X attempts=", firstRc);
        for (size_t i = 0; i < LaunchStorageCount; i++) {
            if (i > 0) printf(",");
            printf("%s:0x%X", storageNames[i], attemptRcs[i]);
        }
        printf("\n");
        return;
    }
    printf("OK action=launched pid=%016lX titleId=%016lX storage=%s\n",
           pid, titleId, usedStorageName);
    fflush(stdout);
}

bool systemCommandsDispatch(int argc, char** argv)
{
    if (argc <= 0) return false;
    const char* command = argv[0];
    bool known = !strcmp(command, "systemCapabilities") || !strcmp(command, "systemInfo")
        || !strcmp(command, "systemTime") || !strcmp(command, "powerStatus")
        || !strcmp(command, "storageStatus") || !strcmp(command, "networkStatus")
        || !strcmp(command, "networkProfile") || !strcmp(command, "accountStatus")
        || !strcmp(command, "applicationStatus") || !strcmp(command, "processList")
        || !strcmp(command, "systemReboot") || !strcmp(command, "systemRebootEmuMMC")
        || !strcmp(command, "systemShutdown")
        || !strcmp(command, "systemSleep") || !strcmp(command, "networkSet")
        || !strcmp(command, "lockScreenStatus") || !strcmp(command, "lockScreenSet")
        || !strcmp(command, "applicationTerminate")
        || !strcmp(command, "audioVolume") || !strcmp(command, "audioMute")
        || !strcmp(command, "gameLaunchHeadless");
    if (!known) return false;

    if (!strcmp(command, "gameLaunchHeadless")) {
        u64 titleId = 0;
        if (argc != 2 || !parseTitleId(argv[1], &titleId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameLaunchHeadlessCommand(titleId);
        return true;
    }
    if (!strcmp(command, "processList")) {
        u64 offset = 0, count = 0;
        if (argc != 3 || !tryParseStringToInt(argv[1], &offset)
            || !tryParseStringToInt(argv[2], &count) || count < 1 || count > PROCESS_PAGE_MAX) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        processList(offset, count);
        return true;
    }
    if (!strcmp(command, "networkSet")) {
        if (argc != 2 || (strcmp(argv[1], "enabled") && strcmp(argv[1], "disabled"))) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        networkSet(!strcmp(argv[1], "enabled"));
        return true;
    }
    if (!strcmp(command, "lockScreenSet")) {
        if (argc != 2 || (strcmp(argv[1], "enabled") && strcmp(argv[1], "disabled"))) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        lockScreenSet(!strcmp(argv[1], "enabled"));
        return true;
    }
    if (!strcmp(command, "audioVolume")) {
        u64 volume = 0;
        bool set = false;
        if (argc == 2) {
            if (!tryParseStringToInt(argv[1], &volume) || volume > 100) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return true;
            }
            set = true;
        }
        else if (argc != 1) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        audioVolumeCommand(set, volume);
        return true;
    }
    if (!strcmp(command, "audioMute")) {
        if (argc == 2 && strcmp(argv[1], "enabled") && strcmp(argv[1], "disabled")) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        if (argc > 2) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        audioMuteCommand(argc == 2 ? argv[1] : NULL);
        return true;
    }
    if (argc != 1) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return true;
    }

    if (!strcmp(command, "systemCapabilities"))
        printf("OK version=1 queries=systemInfo,systemTime,powerStatus,storageStatus,networkStatus,networkProfile,accountStatus,applicationStatus,processList,lockScreenStatus actions=systemReboot,systemRebootEmuMMC,systemShutdown,systemSleep,networkSet,lockScreenSet,applicationTerminate game=launchHeadless processPageMax=%d audio=volume,mute sensitiveData=serial,account,wifiPassphrase authentication=none sleep=experimental rebootEmuMMCMariko=validated rebootEmuMMCErista=experimental\n", PROCESS_PAGE_MAX);
    else if (!strcmp(command, "systemInfo")) systemInfo();
    else if (!strcmp(command, "systemTime")) systemTimeCommand();
    else if (!strcmp(command, "powerStatus")) powerStatus();
    else if (!strcmp(command, "storageStatus")) storageStatus();
    else if (!strcmp(command, "networkStatus")) networkStatus();
    else if (!strcmp(command, "networkProfile")) networkProfile();
    else if (!strcmp(command, "lockScreenStatus")) lockScreenStatus();
    else if (!strcmp(command, "accountStatus")) accountStatus();
    else if (!strcmp(command, "applicationStatus")) applicationStatus();
    else if (!strcmp(command, "systemReboot")) powerAction(true);
    else if (!strcmp(command, "systemRebootEmuMMC")) rebootEmuMmc();
    else if (!strcmp(command, "systemShutdown")) powerAction(false);
    else if (!strcmp(command, "systemSleep")) {
        printf("OK action=sleeping\n");
        fflush(stdout);
        svcSleepSystem();
    }
    else if (!strcmp(command, "applicationTerminate")) applicationTerminate();
    return true;
}
