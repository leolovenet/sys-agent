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

static bool parseHexBytes(const char* text, size_t count, u8* out)
{
    if (text == NULL || strlen(text) != count * 2)
        return false;
    for (size_t i = 0; i < count; i++) {
        unsigned hi = 0, lo = 0;
        char c = text[i * 2];
        if (c >= '0' && c <= '9') hi = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') hi = (unsigned)(c - 'A') + 10;
        else return false;
        c = text[i * 2 + 1];
        if (c >= '0' && c <= '9') lo = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') lo = (unsigned)(c - 'A') + 10;
        else return false;
        out[i] = (u8)((hi << 4) | lo);
    }
    return true;
}

static Result resolveUpdateStorage(u64 titleId, NcmStorageId* outStorage)
{
    /* Application updates are separate Patch titles registered as
     * titleId | 0x800. pmshellLaunchProgram must be given the storage whose
     * content meta database holds that Patch: on firmware 20.0.0+ fsp
     * resolves the launched program's code NCA for the requested storage,
     * so launching with the base title's storage alone loads the base build
     * instead of the update. */
    static const NcmStorageId candidates[] = {
        NcmStorageId_SdCard, NcmStorageId_BuiltInUser, NcmStorageId_GameCard,
    };
    const u64 updateTitleId = titleId | 0x800;

    Result rc = ncmInitialize();
    if (R_FAILED(rc))
        return rc;
    Result lastRc = rc;
    u32 bestVersion = 0;
    bool found = false;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        NcmContentMetaDatabase db;
        rc = ncmOpenContentMetaDatabase(&db, candidates[i]);
        if (R_FAILED(rc)) {
            lastRc = rc;
            continue;
        }
        NcmContentMetaKey key = {0};
        rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, updateTitleId);
        ncmContentMetaDatabaseClose(&db);
        if (R_SUCCEEDED(rc) && key.type == NcmContentMetaType_Patch) {
            if (!found || key.version > bestVersion) {
                found = true;
                bestVersion = key.version;
                *outStorage = candidates[i];
            }
        } else {
            lastRc = rc;
        }
    }
    ncmExit();
    return found ? 0 : lastRc;
}

static Result resolvePatchProgramContentId(u64 titleId, NcmStorageId storage, NcmContentId* outContentId, char* outContentIdHex)
{
    /* Fetch the update's Program content id from the storage's ncm database so
     * the corresponding NCA file on the storage can be located (registered
     * content ids are stored in <8 hex>/<16 hex>.nca folders). */
    NcmContentId contentId = {0};
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return rc;
    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, storage);
    if (R_FAILED(rc)) {
        ncmExit();
        return rc;
    }
    NcmContentMetaKey key = {0};
    rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, titleId | 0x800);
    if (R_SUCCEEDED(rc))
        rc = ncmContentMetaDatabaseGetContentIdByType(&db, &contentId, &key, NcmContentType_Program);
    ncmContentMetaDatabaseClose(&db);
    ncmExit();
    if (R_SUCCEEDED(rc)) {
        if (outContentId != NULL)
            *outContentId = contentId;
        if (outContentIdHex != NULL)
            for (size_t i = 0; i < sizeof(contentId.c); i++)
                sprintf(outContentIdHex + i * 2, "%02X", contentId.c[i]);
    }
    return rc;
}

typedef struct {
    u8 rights_id[0x10];
    u8 access_key[0x10];
} FsRegisterExternalKeyIn;

/* fsp-srv 607 RegisterExternalKey: 0x10-byte rights id + 0x10-byte access key
 * (the decrypted titlekey). Nintendo's fs consults this table whenever an NCA
 * header carries a rights id, which is exactly what the pm-direct headless
 * launch path is missing: the official am/ns launch registers these keys
 * before calling pm:shell, while a raw pmshellLaunchProgram does not. */
static Result fsRegisterExternalKey(const u8 rightsId[0x10], const u8 accessKey[0x10])
{
    Result rc = fsInitialize();
    if (R_FAILED(rc))
        return rc;

    FsRegisterExternalKeyIn in;
    memcpy(in.rights_id, rightsId, 0x10);
    memcpy(in.access_key, accessKey, 0x10);
    rc = serviceDispatchIn(fsGetServiceSession(), 607, in);

    fsExit();
    return rc;
}

/* Resolve the rights id of the update's Program content through the ncm
 * content storage (ncm caches the rights id read from the NCA header at
 * install/registration time). */
static Result resolvePatchRightsId(NcmStorageId storage, const NcmContentId* contentId, u8 outRightsId[0x10])
{
    Result rc = ncmInitialize();
    if (R_FAILED(rc))
        return rc;

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, storage);
    if (R_SUCCEEDED(rc)) {
        NcmRightsId rightsId = {0};
        rc = ncmContentStorageGetRightsIdFromContentId(&cs, &rightsId, contentId, FsContentAttributes_None);
        ncmContentStorageClose(&cs);
        if (R_SUCCEEDED(rc))
            memcpy(outRightsId, rightsId.rights_id.c, 0x10);
    }

    ncmExit();
    return rc;
}

/* --- temporary diagnostic probes for the headless-launch rights investigation --- */

/* One es request on a freshly opened session. The es service closes the
 * session on malformed requests (kernel ConnectionClosed), so every probe
 * variant must get its own handle. Labels/results only; never key data. */
static Result esProbe(const char* label, u32 cmd, const void* inData, size_t inSize,
                      void* outInline, size_t outInlineSize,
                      const void* buf0, size_t buf0Size, u32 buf0Attrs)
{
    Service es;
    Result rc = smGetService(&es, "es");
    if (R_FAILED(rc)) {
        printf(" %s=0x%X", label, rc);
        return rc;
    }

    SfDispatchParams params = { .buffer_attrs = { buf0Attrs } };
    if (buf0 != NULL)
        params.buffers[0] = (SfBuffer){ buf0, buf0Size };
    rc = serviceDispatchImpl(&es, cmd, inData, inSize, outInline, outInlineSize, params);
    printf(" %s=0x%X", label, rc);
    serviceClose(&es);
    return rc;
}

static void gameExternalKeyProbeCommand(u64 titleId)
{
    NcmStorageId updateStorage = NcmStorageId_None;
    Result rc = resolveUpdateStorage(titleId, &updateStorage);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=updateStorage result=0x%X\n", rc);
        return;
    }
    NcmContentId contentId = {0};
    char contentIdHex[0x21] = {0};
    rc = resolvePatchProgramContentId(titleId, updateStorage, &contentId, contentIdHex);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=patchContentId result=0x%X\n", rc);
        return;
    }
    u8 rightsId[0x10];
    rc = resolvePatchRightsId(updateStorage, &contentId, rightsId);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=rightsId result=0x%X\n", rc);
        return;
    }

    printf("OK updateStorage=%u patchProgram=%s rightsId=", updateStorage, contentIdHex);
    printHex(rightsId, sizeof(rightsId));

    u64 size = 0;
    rc = esProbe("es14_size", 14, rightsId, 0x10, &size, sizeof(size), NULL, 0, 0);
    if (R_SUCCEEDED(rc))
        printf(" size=%lu", (unsigned long)size);

    size = 0;
    u8 ticketBuf[0x400];
    rc = esProbe("es16_data", 16, rightsId, 0x10, &size, sizeof(size),
                 ticketBuf, sizeof(ticketBuf), SfBufferAttr_HipcMapAlias | SfBufferAttr_Out);
    if (R_SUCCEEDED(rc))
        printf(" size=%lu", (unsigned long)size);

    /* fsp-srv access probe: register a dummy rights id (all zeroes never
     * matches a real NCA) and immediately unregister it again. This confirms
     * whether command 607 is callable by sys-agent without touching real keys. */
    Result fsRc = fsInitialize();
    if (R_SUCCEEDED(fsRc)) {
        struct {
            u8 rights_id[0x10];
            u8 access_key[0x10];
        } regIn = {0};
        fsRc = serviceDispatchIn(fsGetServiceSession(), 607, regIn);
        printf(" fsp607_dummy=0x%X", fsRc);
        if (R_SUCCEEDED(fsRc)) {
            struct { u8 rights_id[0x10]; } unregIn = {0};
            Result unregRc = serviceDispatchIn(fsGetServiceSession(), 617, unregIn);
            printf(" fsp617_cleanup=0x%X", unregRc);
        }
        fsExit();
    } else {
        printf(" fsp607_init=0x%X", fsRc);
    }

    printf("\n");
    fflush(stdout);
}

/* Read the update's common ticket via es 16 and hand it to the host. The
 * ticket contains the encrypted titlekey; the host decrypts it with its local
 * titlekek and sends the plaintext key back through gameExternalKeyRegister.
 * The encrypted ticket is not a secret key, but keep it out of logs anyway. */
static void gameTicketReadCommand(u64 titleId)
{
    NcmStorageId updateStorage = NcmStorageId_None;
    Result rc = resolveUpdateStorage(titleId, &updateStorage);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=updateStorage result=0x%X\n", rc);
        return;
    }
    NcmContentId contentId = {0};
    char contentIdHex[0x21] = {0};
    rc = resolvePatchProgramContentId(titleId, updateStorage, &contentId, contentIdHex);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=patchContentId result=0x%X\n", rc);
        return;
    }
    u8 rightsId[0x10];
    rc = resolvePatchRightsId(updateStorage, &contentId, rightsId);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=rightsId result=0x%X\n", rc);
        return;
    }

    Service es;
    rc = smGetService(&es, "es");
    if (R_FAILED(rc)) {
        printServiceError("es", rc);
        return;
    }

    FsRightsId rid;
    memcpy(rid.c, rightsId, 0x10);
    u64 size = 0;
    rc = serviceDispatchInOut(&es, 14, rid, size);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=ticketSize result=0x%X\n", rc);
        serviceClose(&es);
        return;
    }

    u8 ticket[0x400] = {0};
    rc = serviceDispatchInOut(&es, 16, rid, size,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { ticket, sizeof(ticket) } });
    serviceClose(&es);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=ticketData result=0x%X\n", rc);
        return;
    }
    if (size == 0 || size > sizeof(ticket)) {
        printf("ERR code=COMMAND_FAILED stage=ticketData result=0x0\n");
        return;
    }

    printf("OK updateStorage=%u patchProgram=%s rightsId=", updateStorage, contentIdHex);
    printHex(rightsId, sizeof(rightsId));
    printf(" ticketSize=%lu ticket=", (unsigned long)size);
    for (u64 i = 0; i < size; i++)
        printf("%02X", ticket[i]);
    printf("\n");
    fflush(stdout);
}

static void gameExternalKeyRegisterCommand(const char* rightsIdHex, const char* keyHex)
{
    u8 rightsId[0x10];
    u8 key[0x10];
    if (!parseHexBytes(rightsIdHex, 0x10, rightsId) || !parseHexBytes(keyHex, 0x10, key)) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    Result rc = fsRegisterExternalKey(rightsId, key);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=registerExternalKey result=0x%X\n", rc);
        return;
    }
    printf("OK action=externalKeyRegistered rightsId=%s\n", rightsIdHex);
    fflush(stdout);
}

static void gameTicketListAllCommand(u64 titleId)
{
    NcmStorageId updateStorage = NcmStorageId_None;
    Result rc = resolveUpdateStorage(titleId, &updateStorage);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=updateStorage result=0x%X\n", rc);
        return;
    }
    NcmContentId contentId = {0};
    char contentIdHex[0x21] = {0};
    rc = resolvePatchProgramContentId(titleId, updateStorage, &contentId, contentIdHex);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=patchContentId result=0x%X\n", rc);
        return;
    }
    u8 rightsId[0x10];
    rc = resolvePatchRightsId(updateStorage, &contentId, rightsId);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=rightsId result=0x%X\n", rc);
        return;
    }

    Service es;
    rc = smGetService(&es, "es");
    if (R_FAILED(rc)) {
        printServiceError("es", rc);
        return;
    }

    s32 commonCount = 0;
    rc = serviceDispatchOut(&es, 9, commonCount);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=countCommon result=0x%X\n", rc);
        serviceClose(&es);
        return;
    }

    s32 personalizedCount = 0;
    rc = serviceDispatchOut(&es, 10, personalizedCount);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=countPersonalized result=0x%X\n", rc);
        serviceClose(&es);
        return;
    }

    enum { MAX_TICKETS = 0x100 };
    FsRightsId commonIds[MAX_TICKETS];
    FsRightsId personalizedIds[MAX_TICKETS];
    s32 maxCommon = commonCount > MAX_TICKETS ? MAX_TICKETS : commonCount;
    s32 maxPersonalized = personalizedCount > MAX_TICKETS ? MAX_TICKETS : personalizedCount;
    struct { s32 num_rights_ids_written; } listOut = {0};

    rc = serviceDispatchInOut(&es, 11, maxCommon, listOut,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { commonIds, (size_t)maxCommon * sizeof(FsRightsId) } });
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=listCommon result=0x%X\n", rc);
        serviceClose(&es);
        return;
    }
    s32 commonWritten = listOut.num_rights_ids_written;

    listOut.num_rights_ids_written = 0;
    rc = serviceDispatchInOut(&es, 12, maxPersonalized, listOut,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { personalizedIds, (size_t)maxPersonalized * sizeof(FsRightsId) } });
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=listPersonalized result=0x%X\n", rc);
        serviceClose(&es);
        return;
    }
    s32 personalizedWritten = listOut.num_rights_ids_written;

    int commonHas = 0, personalizedHas = 0;
    for (s32 i = 0; i < commonWritten && i < MAX_TICKETS; i++) {
        if (!memcmp(commonIds[i].c, rightsId, 0x10)) { commonHas = 1; break; }
    }
    for (s32 i = 0; i < personalizedWritten && i < MAX_TICKETS; i++) {
        if (!memcmp(personalizedIds[i].c, rightsId, 0x10)) { personalizedHas = 1; break; }
    }

    printf("OK updateStorage=%u patchProgram=%s rightsId=", updateStorage, contentIdHex);
    printHex(rightsId, sizeof(rightsId));
    printf(" commonCount=%d commonHasUpdate=%d personalizedCount=%d personalizedHasUpdate=%d",
           commonCount, commonHas, personalizedCount, personalizedHas);

    if (personalizedHas) {
        FsRightsId rid;
        memcpy(rid.c, rightsId, 0x10);
        u64 size = 0;
        rc = serviceDispatchInOut(&es, 15, rid, size);
        if (R_FAILED(rc)) {
            printf(" personalizedSizeErr=0x%X", rc);
        } else {
            u8 ticket[0x400] = {0};
            rc = serviceDispatchInOut(&es, 17, rid, size,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { ticket, sizeof(ticket) } });
            if (R_FAILED(rc)) {
                printf(" personalizedDataErr=0x%X", rc);
            } else {
                printf(" personalizedTicketSize=%lu personalizedTicket=", (unsigned long)size);
                for (u64 i = 0; i < size && i < sizeof(ticket); i++)
                    printf("%02X", ticket[i]);
            }
        }
    }
    printf("\n");
    fflush(stdout);
    serviceClose(&es);
}

static void gameExternalKeyUnregisterCommand(const char* rightsIdHex)
{
    u8 rightsId[0x10];
    if (!parseHexBytes(rightsIdHex, 0x10, rightsId)) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    Result rc = fsInitialize();
    if (R_FAILED(rc)) {
        printServiceError("fsp-srv", rc);
        return;
    }
    struct { u8 rights_id[0x10]; } in;
    memcpy(in.rights_id, rightsId, 0x10);
    rc = serviceDispatchIn(fsGetServiceSession(), 617, in);
    fsExit();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=unregisterExternalKey result=0x%X\n", rc);
        return;
    }
    printf("OK action=externalKeyUnregistered rightsId=%s\n", rightsIdHex);
    fflush(stdout);
}

static void gameNcaProbeCommand(const char* contentIdHex)
{
    NcmContentId contentId = {0};
    if (!parseHexBytes(contentIdHex, sizeof(contentId.c), contentId.c)) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=ncmInit result=0x%X\n", rc);
        return;
    }
    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=contentStorage result=0x%X\n", rc);
        ncmExit();
        return;
    }
    u8 header[0x240];
    rc = ncmContentStorageReadContentIdFile(&cs, header, sizeof(header), &contentId, 0);
    ncmContentStorageClose(&cs);
    ncmExit();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=readNca result=0x%X\n", rc);
        return;
    }
    printf("OK content=%s magic=", contentIdHex);
    printHex(header, 4);
    printf(" rightsId=");
    printHex(header + 0x210, 0x10);
    printf(" header200=");
    printHex(header + 0x200, 0x30);
    printf("\n");
    fflush(stdout);
}

/* Read a window of the registered NCA through ncm/fsp (the NAX0-decrypted
 * view; internal sections stay titlekey-encrypted). Used to compare whether
 * the stored NCA bytes change across boots. */
static void gameNcaDumpCommand(const char* contentIdHex, const char* offsetText, const char* sizeText)
{
    NcmContentId contentId = {0};
    u64 offset = 0, size = 0;
    if (!parseHexBytes(contentIdHex, sizeof(contentId.c), contentId.c) ||
        !tryParseStringToInt(offsetText, &offset) ||
        !tryParseStringToInt(sizeText, &size) ||
        size == 0 || size > 0x4000) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=ncmInit result=0x%X\n", rc);
        return;
    }
    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=contentStorage result=0x%X\n", rc);
        ncmExit();
        return;
    }
    u8 buf[0x4000];
    rc = ncmContentStorageReadContentIdFile(&cs, buf, (size_t)size, &contentId, offset);
    ncmContentStorageClose(&cs);
    ncmExit();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=readNca result=0x%X\n", rc);
        return;
    }
    printf("OK content=%s offset=%lX size=%lX data=", contentIdHex,
        (unsigned long)offset, (unsigned long)size);
    printHex(buf, (size_t)size);
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Process-memory pattern scans (fsp-srv / es external-key recovery)   */
/* ------------------------------------------------------------------ */

enum {
    PatternScanMaxMatches = 64,
    PatternScanChunkSize = 0x10000,
};

typedef struct {
    u64 address;
    u8 before[0x20];
    size_t beforeSize;
    u8 after[0x200];
    size_t afterSize;
} PatternScanMatch;

typedef struct {
    const u8* pattern;
    size_t patternSize;
    size_t captureBefore;
    size_t captureAfter;
    PatternScanMatch matches[PatternScanMaxMatches];
    size_t matchCount;
} PatternScanState;

/* Compact FIPS 180-4 SHA-256 (self-contained; no libnx crypto dependency). */
typedef struct {
    u32 state[8];
    u64 bitCount;
    u8 buffer[64];
    size_t bufferLen;
} Sha256Ctx;

static const u8* findBytesIn(const u8* haystack, size_t haystackSize, const u8* needle, size_t needleSize)
{
    if (needleSize == 0 || haystackSize < needleSize)
        return NULL;
    for (size_t i = 0; i + needleSize <= haystackSize; i++)
        if (!memcmp(haystack + i, needle, needleSize))
            return haystack + i;
    return NULL;
}

static Result resolveProcessId(u64 programId, u64* outPid)
{
    Result rc = pmdmntGetProcessId(outPid, programId);
    if (R_SUCCEEDED(rc) && *outPid != 0)
        return rc;
    /* Kernel-package programs (e.g. fs = 0x0100000000000000) are not always
     * resolvable through pm:dmnt; enumerate the kernel process list and match
     * the CreateProcess debug event, exactly like nxdumptool. */
    enum { MaxPids = 0x100 };
    u64 pids[MaxPids];
    s32 numProcesses = 0;
    rc = svcGetProcessList(&numProcesses, pids, MaxPids);
    if (R_FAILED(rc))
        return rc;
    for (s32 i = 0; i < numProcesses; i++) {
        Handle debugHandle = 0;
        rc = svcDebugActiveProcess(&debugHandle, pids[i]);
        if (R_FAILED(rc))
            continue;
        DebugEventInfo event = {0};
        Result eventRc = svcGetDebugEvent(&event, debugHandle);
        svcCloseHandle(debugHandle);
        if (R_SUCCEEDED(eventRc) && event.type == DebugEventType_CreateProcess &&
            event.info.create_process.program_id == programId) {
            *outPid = pids[i];
            return 0;
        }
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

static Result scanProcessMemoryForPattern(u64 processId, PatternScanState* state)
{
    Handle debugHandle = 0;
    Result rc = svcDebugActiveProcess(&debugHandle, processId);
    if (R_FAILED(rc))
        return rc;

    u64 addr = 0;
    while (state->matchCount < PatternScanMaxMatches) {
        MemoryInfo info = {0};
        u32 pageInfo = 0;
        rc = svcQueryDebugProcessMemory(&info, &pageInfo, debugHandle, addr);
        if (R_FAILED(rc))
            break;
        u8 memType = (u8)(info.type & 0xFF);
        bool readable = memType != MemType_Unmapped && memType != MemType_Reserved &&
                        memType != MemType_Io && memType != MemType_ThreadLocal &&
                        info.perm != 0 && info.size > 0;
        if (readable) {
            u64 regionAddr = info.addr;
            u64 regionSize = info.size;
            u64 off = 0;
            while (off < regionSize && state->matchCount < PatternScanMaxMatches) {
                size_t want = PatternScanChunkSize;
                if ((u64)want > regionSize - off)
                    want = (size_t)(regionSize - off);
                static u8 chunk[PatternScanChunkSize];
                rc = svcReadDebugProcessMemory(chunk, debugHandle, regionAddr + off, want);
                if (R_FAILED(rc))
                    break;
                const u8* pos = chunk;
                size_t remaining = want;
                while (remaining >= state->patternSize && state->matchCount < PatternScanMaxMatches) {
                    const u8* hit = findBytesIn(pos, remaining, state->pattern, state->patternSize);
                    if (!hit)
                        break;
                    u64 hitAddr = regionAddr + off + (size_t)(hit - chunk);
                    PatternScanMatch* match = &state->matches[state->matchCount];
                    memset(match, 0, sizeof(*match));
                    match->address = hitAddr;
                    state->matchCount++;

                    /* Context after the pattern (clamped to this region). */
                    u64 afterAddr = hitAddr + state->patternSize;
                    u64 afterMax = regionAddr + regionSize;
                    if (afterAddr < afterMax) {
                        size_t afterWant = state->captureAfter;
                        if ((u64)afterWant > afterMax - afterAddr)
                            afterWant = (size_t)(afterMax - afterAddr);
                        if (afterWant > sizeof(match->after))
                            afterWant = sizeof(match->after);
                        if (R_SUCCEEDED(svcReadDebugProcessMemory(match->after, debugHandle, afterAddr, afterWant)))
                            match->afterSize = afterWant;
                    }
                    /* Context before the pattern (clamped to this region). */
                    if (hitAddr >= regionAddr) {
                        size_t beforeWant = state->captureBefore;
                        u64 beforeMax = hitAddr - regionAddr;
                        if ((u64)beforeWant > beforeMax)
                            beforeWant = (size_t)beforeMax;
                        if (beforeWant > sizeof(match->before))
                            beforeWant = sizeof(match->before);
                        if (beforeWant > 0 &&
                            R_SUCCEEDED(svcReadDebugProcessMemory(match->before, debugHandle,
                                hitAddr - beforeWant, beforeWant)))
                            match->beforeSize = beforeWant;
                    }

                    pos = hit + 1;
                    remaining = want - (size_t)(hit - chunk) - 1;
                }
                off += want;
            }
        }
        if (info.size == 0)
            break;
        u64 next = info.addr + info.size;
        if (next <= info.addr)
            break;
        addr = next;
    }
    svcCloseHandle(debugHandle);
    return 0;
}

static void printPatternMatches(const char* label, const PatternScanState* state)
{
    printf(" %s=%zu", label, state->matchCount);
    for (size_t i = 0; i < state->matchCount; i++) {
        printf(",%zu@%lX:pre=", i, state->matches[i].address);
        printHex(state->matches[i].before, state->matches[i].beforeSize);
        printf(",post=");
        printHex(state->matches[i].after, state->matches[i].afterSize);
    }
}

static void gameExternalKeyScanCommand(const char* rightsIdHex, const char* targetName)
{
    u8 rightsId[0x10];
    if (!parseHexBytes(rightsIdHex, sizeof(rightsId), rightsId)) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    u64 programId = 0;
    const char* targetLabel = targetName;
    if (!strcmp(targetName, "fs")) {
        programId = 0x0100000000000000ULL;
    } else if (!strcmp(targetName, "es")) {
        programId = 0x0100000000000033ULL;
    } else {
        if (!tryParseStringToInt(targetName, &programId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return;
        }
    }
    u64 pid = 0;
    Result rc = resolveProcessId(programId, &pid);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=resolvePid target=%s result=0x%X\n", targetLabel, rc);
        return;
    }
    static PatternScanState state; /* commands run serially on the main thread */
    memset(&state, 0, sizeof(state));
    state.pattern = rightsId;
    state.patternSize = sizeof(rightsId);
    state.captureBefore = 0x10;
    state.captureAfter = 0x20;
    rc = scanProcessMemoryForPattern(pid, &state);
    printf("OK target=%s pid=%lX scanRc=0x%X", targetLabel, pid, rc);
    printPatternMatches("matches", &state);
    printf("\n");
    fflush(stdout);
}

/* Dump a window of an arbitrary process (fs/es/<pid>) through the debug
 * syscalls. Used to locate/read the real ticket structures inside es and to
 * inspect the fsp-srv external-key table. size is capped at 0x4000 per call. */
static void gameMemDumpCommand(const char* targetName, const char* addrText, const char* sizeText)
{
    u64 programId = 0;
    const char* targetLabel = targetName;
    if (!strcmp(targetName, "fs")) {
        programId = 0x0100000000000000ULL;
    } else if (!strcmp(targetName, "es")) {
        programId = 0x0100000000000033ULL;
    } else {
        if (!tryParseStringToInt(targetName, &programId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return;
        }
    }
    u64 addr = 0, size = 0;
    if (!tryParseStringToInt(addrText, &addr) || !tryParseStringToInt(sizeText, &size) ||
        size == 0 || size > 0x4000) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    u64 pid = 0;
    Result rc = resolveProcessId(programId, &pid);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=resolvePid target=%s result=0x%X\n", targetLabel, rc);
        return;
    }
    Handle debugHandle = 0;
    rc = svcDebugActiveProcess(&debugHandle, pid);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=debugActive target=%s pid=%lX result=0x%X\n", targetLabel, pid, rc);
        return;
    }
    static u8 buf[0x4000];
    rc = svcReadDebugProcessMemory(buf, debugHandle, addr, (size_t)size);
    svcCloseHandle(debugHandle);
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=readMemory target=%s addr=%lX size=%lX result=0x%X\n", targetLabel, addr, size, rc);
        return;
    }
    printf("OK target=%s pid=%lX addr=%lX size=%lX data=", targetLabel, pid, addr, size);
    printHex(buf, (size_t)size);
    printf("\n");
    fflush(stdout);
}

/* Read a window of a raw BIS partition (System/User/...) through
 * fsOpenBisStorage. Used to locate the es save files on the FAT System
 * partition (sphaira's approach: raw storage + own FAT parser). */
static void gameBisDumpCommand(const char* partitionName, const char* offsetText, const char* sizeText)
{
    FsBisPartitionId partitionId;
    if (!strcmp(partitionName, "System")) {
        partitionId = FsBisPartitionId_System;
    } else if (!strcmp(partitionName, "User")) {
        partitionId = FsBisPartitionId_User;
    } else if (!strcmp(partitionName, "SafeMode")) {
        partitionId = FsBisPartitionId_SafeMode;
    } else {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    u64 offset = 0, size = 0;
    if (!tryParseStringToInt(offsetText, &offset) || !tryParseStringToInt(sizeText, &size) ||
        size == 0 || size > 0x4000) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    FsStorage storage;
    Result rc = fsOpenBisStorage(&storage, partitionId);
    if (R_FAILED(rc)) {
        printCommandError("fsOpenBisStorage", rc);
        return;
    }
    s64 storageSize = 0;
    fsStorageGetSize(&storage, &storageSize);
    static u8 buf[0x4000];
    rc = fsStorageRead(&storage, (s64)offset, buf, (size_t)size);
    fsStorageClose(&storage);
    if (R_FAILED(rc)) {
        printCommandError("fsStorageRead", rc);
        return;
    }
    printf("OK partition=%s size=%lX offset=%lX data=", partitionName, (unsigned long)storageSize, offset);
    printHex(buf, (size_t)size);
    printf("\n");
    fflush(stdout);
}

/* Scan a raw BIS partition region for a byte pattern on-device. Used to
 * locate plaintext ticket data (issuer signatures) inside the es save files
 * without porting the full SaveFS parser. size capped at 0x20000000. */
static void gameBisScanCommand(const char* partitionName, const char* offsetText, const char* sizeText, const char* patternHex)
{
    FsBisPartitionId partitionId;
    if (!strcmp(partitionName, "System")) {
        partitionId = FsBisPartitionId_System;
    } else if (!strcmp(partitionName, "User")) {
        partitionId = FsBisPartitionId_User;
    } else if (!strcmp(partitionName, "SafeMode")) {
        partitionId = FsBisPartitionId_SafeMode;
    } else {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    u64 offset = 0, size = 0;
    if (!tryParseStringToInt(offsetText, &offset) || !tryParseStringToInt(sizeText, &size) ||
        size == 0 || size > 0x20000000ULL) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    const size_t patternMax = 0x20;
    u8 pattern[patternMax];
    size_t patternSize = 0;
    size_t hexLen = strlen(patternHex);
    if ((hexLen % 2) != 0 || hexLen / 2 > patternMax) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    for (size_t i = 0; i < hexLen; i += 2) {
        unsigned hi = 0, lo = 0;
        char c = patternHex[i];
        if (c >= '0' && c <= '9') hi = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') hi = (unsigned)(c - 'A') + 10;
        else { printf("ERR code=INVALID_ARGUMENTS\n"); return; }
        c = patternHex[i + 1];
        if (c >= '0' && c <= '9') lo = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') lo = (unsigned)(c - 'A') + 10;
        else { printf("ERR code=INVALID_ARGUMENTS\n"); return; }
        if (patternSize >= patternMax) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return;
        }
        pattern[patternSize++] = (u8)((hi << 4) | lo);
    }
    if (patternSize == 0) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    FsStorage storage;
    Result rc = fsOpenBisStorage(&storage, partitionId);
    if (R_FAILED(rc)) {
        printCommandError("fsOpenBisStorage", rc);
        return;
    }
    enum { ScanChunkSize = 0x40000 };
    static u8 chunk[ScanChunkSize + 0x20];
    u64 matches = 0;
    printf("OK partition=%s matches=", partitionName);
    u64 cur = offset;
    u64 end = offset + size;
    u64 overlap = patternSize - 1;
    u64 carry = 0;
    while (cur < end) {
        u64 want = end - cur;
        if (want > ScanChunkSize)
            want = ScanChunkSize;
        rc = fsStorageRead(&storage, (s64)cur, chunk + carry, (size_t)want);
        if (R_FAILED(rc)) {
            printf("ERR code=COMMAND_FAILED stage=fsStorageRead offset=%lX result=0x%X\n", cur, rc);
            fsStorageClose(&storage);
            return;
        }
        u64 regionSize = carry + want;
        const u8* pos = chunk;
        while (regionSize >= patternSize) {
            const u8* hit = findBytesIn(pos, (size_t)regionSize, pattern, patternSize);
            if (!hit)
                break;
            u64 abs = cur + (u64)(hit - chunk);
            if (matches) printf(",");
            printf("%lu@%lX", (unsigned long)matches, (unsigned long)abs);
            matches++;
            u64 consumed = (u64)(hit - pos) + 1;
            pos = hit + 1;
            regionSize -= consumed;
        }
        if (matches > 0 && matches >= 64) {
            printf("...truncated");
            break;
        }
        /* Carry the tail for cross-chunk matches. */
        carry = 0;
        if (want == ScanChunkSize && overlap > 0) {
            memcpy(chunk, chunk + ScanChunkSize - overlap, overlap);
            carry = overlap;
        }
        cur += want;
    }
    printf("\n");
    fsStorageClose(&storage);
    fflush(stdout);
}

/* spl:es unwrap (common ticket path) + fs 607 register.
 * PrepareCommonEsTitleKey(key_source, generation) -> current-boot AccessKey. */
static void gameExternalKeyPrepareCommonCommand(const char* rightsIdHex, const char* keySourceHex, const char* genText)
{
    u8 rightsId[0x10];
    u8 keySource[0x10];
    u64 gen = 0;
    if (!parseHexBytes(rightsIdHex, sizeof(rightsId), rightsId) ||
        !parseHexBytes(keySourceHex, sizeof(keySource), keySource) ||
        !tryParseStringToInt(genText, &gen) || gen > 0xFF) {
        printf("ERR code=INVALID_ARGUMENTS\n");
        return;
    }
    Result rc = splEsInitialize();
    if (R_FAILED(rc)) {
        printServiceError("spl:es", rc);
        return;
    }
    u8 accessKey[0x10];
    rc = splEsUnwrapAesWrappedTitlekey(keySource, (u32)gen, accessKey);
    splExit();
    if (R_FAILED(rc)) {
        printCommandError("splEsUnwrapAesWrappedTitlekey", rc);
        return;
    }
    rc = fsRegisterExternalKey(rightsId, accessKey);
    if (R_FAILED(rc)) {
        printCommandError("registerExternalKey", rc);
        return;
    }
    printf("OK action=externalKeyPrepared mode=common rightsId=%s accessKey=", rightsIdHex);
    printHex(accessKey, sizeof(accessKey));
    printf("\n");
    fflush(stdout);
}

static Result redirectProgramToPatch(u64 titleId, NcmStorageId storage)
{
    Result rc = lrInitialize();
    if (R_FAILED(rc))
        return rc;
    LrLocationResolver resolver;
    rc = lrOpenLocationResolver(storage, &resolver);
    if (R_FAILED(rc)) {
        lrExit();
        return rc;
    }
    char patchPath[FS_MAX_PATH] = {0};
    rc = lrLrResolveProgramPath(&resolver, titleId | 0x800, patchPath);
    if (R_SUCCEEDED(rc))
        rc = lrLrRedirectProgramPath(&resolver, titleId, patchPath);
    serviceClose(&resolver.s);
    lrExit();
    return rc;
}

static void gameLaunchHeadlessCommand(u64 titleId, const char* requestedStorage)
{
    enum { LaunchStorageCount = 4 };
    static const NcmStorageId launchStorageOrder[LaunchStorageCount] = {
        NcmStorageId_SdCard, NcmStorageId_BuiltInUser,
        NcmStorageId_GameCard, NcmStorageId_None,
    };
    static const char* storageNames[LaunchStorageCount] = {
        "SdCard", "BuiltInUser", "GameCard", "None",
    };

    /* Build the effective storage order. An explicit request uses exactly
     * that storage; otherwise prefer the storage holding the title's update
     * (Patch) so the launched process is the updated build, then fall back
     * through the common storages. */
    NcmStorageId effectiveOrder[LaunchStorageCount + 1];
    const char* effectiveNames[LaunchStorageCount + 1];
    size_t effectiveCount = 0;
    const char* updateStorageName = "none";
    char patchProgramContentId[0x21] = {0};
    NcmContentId patchContentId = {0};
    bool patchResolved = false;
    char rightsIdHex[0x21] = {0};
    char patchRedirectStatus[0x40] = "patchRedirect=none";
    if (requestedStorage != NULL) {
        for (size_t i = 0; i < LaunchStorageCount; i++) {
            if (!strcmp(requestedStorage, storageNames[i])) {
                effectiveOrder[0] = launchStorageOrder[i];
                effectiveNames[0] = storageNames[i];
                effectiveCount = 1;
                break;
            }
        }
        if (effectiveCount == 0) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return;
        }
    } else {
        NcmStorageId updateStorage = NcmStorageId_None;
        if (R_SUCCEEDED(resolveUpdateStorage(titleId, &updateStorage))) {
            for (size_t i = 0; i < LaunchStorageCount; i++) {
                if (launchStorageOrder[i] == updateStorage) {
                    effectiveOrder[effectiveCount] = updateStorage;
                    effectiveNames[effectiveCount] = storageNames[i];
                    updateStorageName = storageNames[i];
                    effectiveCount++;
                    if (R_SUCCEEDED(resolvePatchProgramContentId(titleId, updateStorage, &patchContentId, patchProgramContentId))) {
                        patchResolved = true;
                        u8 rightsId[0x10];
                        if (R_SUCCEEDED(resolvePatchRightsId(updateStorage, &patchContentId, rightsId)))
                            for (size_t i = 0; i < sizeof(rightsId); i++)
                                sprintf(rightsIdHex + i * 2, "%02X", rightsId[i]);
                        Result redirectRc = redirectProgramToPatch(titleId, updateStorage);
                        if (R_SUCCEEDED(redirectRc))
                            snprintf(patchRedirectStatus, sizeof(patchRedirectStatus),
                                "patchRedirect=ok");
                        else
                            snprintf(patchRedirectStatus, sizeof(patchRedirectStatus),
                                "patchRedirect=failed:0x%X", redirectRc);
                    }
                    break;
                }
            }
        }
        for (size_t i = 0; i < LaunchStorageCount; i++) {
            bool already = false;
            for (size_t j = 0; j < effectiveCount; j++) {
                if (effectiveOrder[j] == launchStorageOrder[i]) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                effectiveOrder[effectiveCount] = launchStorageOrder[i];
                effectiveNames[effectiveCount] = storageNames[i];
                effectiveCount++;
            }
        }
    }

    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) {
        printServiceError("pm:shell", rc);
        return;
    }
    const char* usedStorageName = "None";
    Result firstRc = 0;
    Result attemptRcs[LaunchStorageCount + 1] = {0};
    u64 pid = 0;
    for (size_t i = 0; i < effectiveCount; i++) {
        NcmProgramLocation location = { .program_id = titleId, .storageID = effectiveOrder[i] };
        rc = pmshellLaunchProgram(0, &location, &pid);
        attemptRcs[i] = rc;
        if (R_SUCCEEDED(rc)) {
            usedStorageName = effectiveNames[i];
            break;
        }
        if (firstRc == 0)
            firstRc = rc;
    }
    pmshellExit();
    if (R_FAILED(rc)) {
        printf("ERR code=COMMAND_FAILED stage=launchProgram result=0x%X attempts=", firstRc);
        for (size_t i = 0; i < effectiveCount; i++) {
            if (i > 0) printf(",");
            printf("%s:0x%X", effectiveNames[i], attemptRcs[i]);
        }
        if (requestedStorage == NULL) {
            printf(" updateStorage=%s", updateStorageName);
            if (patchProgramContentId[0] != '\0')
                printf(" patchProgram=%s", patchProgramContentId);
            if (patchResolved && rightsIdHex[0] != '\0')
                printf(" rightsId=%s", rightsIdHex);
            printf(" %s", patchRedirectStatus);
        }
        printf("\n");
        return;
    }
    printf("OK action=launched pid=%016lX titleId=%016lX storage=%s", pid, titleId, usedStorageName);
    if (requestedStorage == NULL) {
        printf(" updateStorage=%s", updateStorageName);
        if (patchProgramContentId[0] != '\0')
            printf(" patchProgram=%s", patchProgramContentId);
        if (patchResolved && rightsIdHex[0] != '\0')
            printf(" rightsId=%s", rightsIdHex);
        printf(" %s", patchRedirectStatus);
    }
    printf("\n");
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
        || !strcmp(command, "gameLaunchHeadless")
        || !strcmp(command, "gameExternalKeyProbe")
        || !strcmp(command, "gameTicketRead")
        || !strcmp(command, "gameTicketListAll")
        || !strcmp(command, "gameExternalKeyRegister")
        || !strcmp(command, "gameExternalKeyUnregister")
        || !strcmp(command, "gameExternalKeyScan")
        || !strcmp(command, "gameExternalKeyPrepareCommon")
        || !strcmp(command, "gameMemDump")
        || !strcmp(command, "gameBisDump")
        || !strcmp(command, "gameBisScan")
        || !strcmp(command, "gameNcaProbe")
        || !strcmp(command, "gameNcaDump");
    if (!known) return false;

    if (!strcmp(command, "gameNcaProbe")) {
        if (argc != 2) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameNcaProbeCommand(argv[1]);
        return true;
    }
    if (!strcmp(command, "gameNcaDump")) {
        if (argc != 4) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameNcaDumpCommand(argv[1], argv[2], argv[3]);
        return true;
    }
    if (!strcmp(command, "gameTicketRead")) {
        u64 titleId = 0;
        if (argc != 2 || !parseTitleId(argv[1], &titleId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameTicketReadCommand(titleId);
        return true;
    }
    if (!strcmp(command, "gameTicketListAll")) {
        u64 titleId = 0;
        if (argc != 2 || !parseTitleId(argv[1], &titleId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameTicketListAllCommand(titleId);
        return true;
    }
    if (!strcmp(command, "gameExternalKeyRegister")) {
        if (argc != 3) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameExternalKeyRegisterCommand(argv[1], argv[2]);
        return true;
    }
    if (!strcmp(command, "gameExternalKeyUnregister")) {
        if (argc != 2) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameExternalKeyUnregisterCommand(argv[1]);
        return true;
    }
    if (!strcmp(command, "gameExternalKeyScan")) {
        if (argc != 3) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameExternalKeyScanCommand(argv[1], argv[2]);
        return true;
    }
    if (!strcmp(command, "gameExternalKeyPrepareCommon")) {
        if (argc != 4) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameExternalKeyPrepareCommonCommand(argv[1], argv[2], argv[3]);
        return true;
    }
    if (!strcmp(command, "gameMemDump")) {
        if (argc != 4) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameMemDumpCommand(argv[1], argv[2], argv[3]);
        return true;
    }
    if (!strcmp(command, "gameBisDump")) {
        if (argc != 4) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameBisDumpCommand(argv[1], argv[2], argv[3]);
        return true;
    }
    if (!strcmp(command, "gameBisScan")) {
        if (argc != 5) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameBisScanCommand(argv[1], argv[2], argv[3], argv[4]);
        return true;
    }
    if (!strcmp(command, "gameExternalKeyProbe")) {
        u64 titleId = 0;
        if (argc != 2 || !parseTitleId(argv[1], &titleId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameExternalKeyProbeCommand(titleId);
        return true;
    }
    if (!strcmp(command, "gameLaunchHeadless")) {
        u64 titleId = 0;
        if (argc < 2 || argc > 3 || !parseTitleId(argv[1], &titleId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return true;
        }
        gameLaunchHeadlessCommand(titleId, argc == 3 ? argv[2] : NULL);
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
