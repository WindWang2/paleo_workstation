#include "Engine/StorageProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace seismic {
namespace engine {
namespace {

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

IoProfile MakeProfile(StorageClass storageClass, const std::string& note) {
    IoProfile profile;
    profile.storageClass = storageClass;
    profile.note = note;
    switch(storageClass) {
        case StorageClass::Rotational:
            // Seek-avoiding: deep sequential windows, single queue, big merges.
            profile.sequentialBlockBytes = 32ull * 1024ull * 1024ull;
            profile.readQueueDepth = 1;
            profile.mergeGapBytes = 4ull * 1024ull * 1024ull;
            profile.prefetchDistance = 6;
            profile.checkpointIntervalBytes = 1024ull * 1024ull * 1024ull;
            profile.backgroundIoPercent = 25;
            profile.writeBatchBytes = 32ull * 1024ull * 1024ull;
            break;
        case StorageClass::SataSsd:
            profile.sequentialBlockBytes = 8ull * 1024ull * 1024ull;
            profile.readQueueDepth = 3;
            profile.mergeGapBytes = 512ull * 1024ull;
            profile.prefetchDistance = 3;
            profile.checkpointIntervalBytes = 256ull * 1024ull * 1024ull;
            profile.backgroundIoPercent = 50;
            profile.writeBatchBytes = 8ull * 1024ull * 1024ull;
            break;
        case StorageClass::Nvme:
            // Latency-oriented: small windows, more in-flight reads.
            profile.sequentialBlockBytes = 2ull * 1024ull * 1024ull;
            profile.readQueueDepth = 8;
            profile.mergeGapBytes = 64ull * 1024ull;
            profile.prefetchDistance = 2;
            profile.checkpointIntervalBytes = 128ull * 1024ull * 1024ull;
            profile.backgroundIoPercent = 75;
            profile.writeBatchBytes = 2ull * 1024ull * 1024ull;
            break;
        case StorageClass::Unknown:
        default:
            // Conservative bounded fallback: no device claims, no extremes.
            profile.sequentialBlockBytes = 4ull * 1024ull * 1024ull;
            profile.readQueueDepth = 2;
            profile.mergeGapBytes = 256ull * 1024ull;
            profile.prefetchDistance = 2;
            profile.checkpointIntervalBytes = 256ull * 1024ull * 1024ull;
            profile.backgroundIoPercent = 50;
            profile.writeBatchBytes = 4ull * 1024ull * 1024ull;
            break;
    }
    if(storageClass == StorageClass::Unknown) {
        profile.note = note.empty() ? "fallback" : note;
    }
    return profile;
}

#ifdef _WIN32
const char* BusName(int busType) {
    switch(busType) {
        case BusTypeScsi: return "scsi";
        case BusTypeAtapi: return "atapi";
        case BusTypeAta: return "ata";
        case BusType1394: return "1394";
        case BusTypeSsa: return "ssa";
        case BusTypeFibre: return "fibre";
        case BusTypeUsb: return "usb";
        case BusTypeRAID: return "raid";
        case BusTypeiScsi: return "iscsi";
        case BusTypeSas: return "sas";
        case BusTypeSata: return "sata";
        case BusTypeSd: return "sd";
        case BusTypeMmc: return "mmc";
        case BusTypeVirtual: return "virtual";
        case BusTypeFileBackedVirtual: return "file-backed-virtual";
        case BusTypeSpaces: return "spaces";
        case BusTypeNvme: return "nvme";
        case BusTypeSCM: return "scm";
        case BusTypeUfs: return "ufs";
        default: return "unknown";
    }
}
#endif

#ifdef _WIN32
std::wstring VolumeRootFor(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path probe = ec ? path : absolute;
    wchar_t volume[MAX_PATH] = {};
    if(GetVolumePathNameW(probe.wstring().c_str(), volume, MAX_PATH) == 0) {
        return std::wstring();
    }
    return std::wstring(volume);
}
#endif

} // namespace

const char* StorageClassName(StorageClass storageClass) {
    switch(storageClass) {
        case StorageClass::Rotational: return "HDD";
        case StorageClass::SataSsd: return "SATA SSD";
        case StorageClass::Nvme: return "NVMe";
        case StorageClass::Unknown:
        default: return "Unknown";
    }
}

IoProfile ProfileFor(StorageClass storageClass, const std::string& overrideName) {
    const std::string mode = ToLower(overrideName);
    if(mode.empty() || mode == "auto") {
        return MakeProfile(storageClass, "detected");
    }
    if(mode == "hdd" || mode == "rotational") {
        return MakeProfile(StorageClass::Rotational, "override:" + mode);
    }
    if(mode == "sata" || mode == "satassd" || mode == "ssd") {
        return MakeProfile(StorageClass::SataSsd, "override:" + mode);
    }
    if(mode == "nvme") {
        return MakeProfile(StorageClass::Nvme, "override:" + mode);
    }
    if(mode == "lowmem") {
        IoProfile profile = MakeProfile(StorageClass::Unknown, "override:lowmem");
        profile.readQueueDepth = 1;
        profile.prefetchDistance = 0;
        profile.sequentialBlockBytes = 1024ull * 1024ull;
        profile.checkpointIntervalBytes = 64ull * 1024ull * 1024ull;
        profile.backgroundIoPercent = 25;
        profile.writeBatchBytes = 1024ull * 1024ull;
        return profile;
    }
    return MakeProfile(storageClass, "detected");
}

StorageClass ClassifyPath(const std::filesystem::path& path,
                          std::string* deviceName,
                          std::string* busType,
                          std::string* note) {
    if(deviceName != nullptr) {
        deviceName->clear();
    }
    if(busType != nullptr) {
        busType->clear();
    }
    if(note != nullptr) {
        note->clear();
    }
#ifdef _WIN32
    const std::wstring volume = VolumeRootFor(path);
    if(volume.empty()) {
        if(note != nullptr) {
            *note = "volume path unavailable";
        }
        return StorageClass::Unknown;
    }
    // Strip the trailing backslash: \\.\C:
    std::wstring device = L"\\\\.\\" + volume.substr(0, volume.size() - 1);
    HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        if(note != nullptr) {
            *note = "cannot open volume device";
        }
        return StorageClass::Unknown;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD bytesReturned = 0;
    if(!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &header, sizeof(header), &bytesReturned, nullptr)) {
        CloseHandle(handle);
        if(note != nullptr) {
            *note = "storage property query failed";
        }
        return StorageClass::Unknown;
    }
    std::vector<unsigned char> buffer(header.Size > 0 ? header.Size : sizeof(STORAGE_DEVICE_DESCRIPTOR));
    STORAGE_DEVICE_DESCRIPTOR* descriptor =
        reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if(!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr)) {
        CloseHandle(handle);
        if(note != nullptr) {
            *note = "storage descriptor query failed";
        }
        return StorageClass::Unknown;
    }
    const int bus = descriptor->BusType;
    if(deviceName != nullptr && descriptor->ProductIdOffset != 0) {
        const char* product = reinterpret_cast<const char*>(buffer.data() + descriptor->ProductIdOffset);
        *deviceName = product;
    }
    if(busType != nullptr) {
        *busType = BusName(bus);
    }
    // Seek penalty tells rotational media apart regardless of the bus.
    STORAGE_PROPERTY_QUERY seekQuery{};
    seekQuery.PropertyId = StorageDeviceSeekPenaltyProperty;
    seekQuery.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    bool incursSeekPenalty = false;
    bool haveSeekAnswer = false;
    if(DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &seekQuery, sizeof(seekQuery),
                       &seek, sizeof(seek), &bytesReturned, nullptr)) {
        incursSeekPenalty = seek.IncursSeekPenalty != FALSE;
        haveSeekAnswer = true;
    }
    CloseHandle(handle);
    if(haveSeekAnswer && incursSeekPenalty) {
        if(note != nullptr) {
            *note = "storage seek-penalty query";
        }
        return StorageClass::Rotational;
    }
    if(bus == BusTypeNvme || bus == BusTypeSCM || bus == BusTypeUfs) {
        if(note != nullptr) {
            *note = "storage bus query";
        }
        return StorageClass::Nvme;
    }
    if(bus == BusTypeSata || bus == BusTypeSas) {
        if(note != nullptr) {
            *note = "storage bus query";
        }
        return StorageClass::SataSsd;
    }
    if(haveSeekAnswer) {
        if(note != nullptr) {
            *note = "no seek penalty but unknown bus";
        }
        return StorageClass::Unknown;
    }
    if(note != nullptr) {
        *note = "no seek-penalty answer";
    }
    return StorageClass::Unknown;
#else
    // POSIX: stat the path, walk /sys/dev/block/<major>:<minor> for the owning
    // device, then read queue/rotational and the device name.
    struct stat st {};
    const std::string utf8 = path.string();
    if(::stat(utf8.c_str(), &st) != 0) {
        if(note != nullptr) {
            *note = "stat failed";
        }
        return StorageClass::Unknown;
    }
    const std::string sysDev = "/sys/dev/block/" + std::to_string(major(st.st_dev)) +
                               ":" + std::to_string(minor(st.st_dev));
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::canonical(sysDev, ec);
    const std::filesystem::path sysRoot = ec ? std::filesystem::path(sysDev) : resolved;
    const std::string devName = sysRoot.filename().string();
    if(deviceName != nullptr) {
        *deviceName = devName;
    }
    if(busType != nullptr) {
        *busType = devName.rfind("nvme", 0) == 0 ? "nvme"
            : devName.rfind("sd", 0) == 0 ? "sata"
            : devName.rfind("vd", 0) == 0 ? "virtual"
            : devName.rfind("xvd", 0) == 0 ? "virtual"
            : devName.rfind("dm-", 0) == 0 ? "dm"
            : devName.rfind("md", 0) == 0 ? "raid" : "";
    }
    std::ifstream in(sysRoot / "queue" / "rotational");
    int rotational = -1;
    if(!(in >> rotational)) {
        if(note != nullptr) {
            *note = "no sysfs rotational flag";
        }
        return StorageClass::Unknown;
    }
    if(note != nullptr) {
        *note = "sysfs";
    }
    if(devName.rfind("nvme", 0) == 0) {
        return StorageClass::Nvme;
    }
    return rotational == 1 ? StorageClass::Rotational : StorageClass::SataSsd;
#endif
}

std::string StorageProfileOverride() {
    const char* value = std::getenv("SEISMIC_STORAGE_PROFILE");
    return value == nullptr ? std::string() : std::string(value);
}

StorageProfileSet ResolveStorageProfiles(const std::filesystem::path& sourcePath,
                                         const std::filesystem::path& workspacePath,
                                         const std::filesystem::path& indexCachePath,
                                         const std::string& overrideName) {
    const std::string override = overrideName.empty() ? StorageProfileOverride() : overrideName;
    StorageProfileSet set;
    set.sourcePath = sourcePath.u8string();
    set.workspacePath = workspacePath.u8string();
    set.indexCachePath = indexCachePath.u8string();
    {
        std::string device;
        std::string bus;
        std::string note;
        const StorageClass storageClass = ClassifyPath(sourcePath, &device, &bus, &note);
        set.source = ProfileFor(storageClass, override);
        set.source.deviceName = device;
        set.source.busType = bus;
        set.source.detected = storageClass != StorageClass::Unknown;
        if(!note.empty() && set.source.note == "detected") {
            set.source.note = note;
        }
    }
    {
        std::string device;
        std::string bus;
        std::string note;
        const StorageClass storageClass = ClassifyPath(workspacePath, &device, &bus, &note);
        set.workspace = ProfileFor(storageClass, override);
        set.workspace.deviceName = device;
        set.workspace.busType = bus;
        set.workspace.detected = storageClass != StorageClass::Unknown;
        if(!note.empty() && set.workspace.note == "detected") {
            set.workspace.note = note;
        }
    }
    {
        std::string device;
        std::string bus;
        std::string note;
        const StorageClass storageClass = ClassifyPath(indexCachePath, &device, &bus, &note);
        set.indexCache = ProfileFor(storageClass, override);
        set.indexCache.deviceName = device;
        set.indexCache.busType = bus;
        set.indexCache.detected = storageClass != StorageClass::Unknown;
        if(!note.empty() && set.indexCache.note == "detected") {
            set.indexCache.note = note;
        }
    }
    if(!override.empty()) {
        set.source.note = "override:" + ToLower(override);
        set.workspace.note = "override:" + ToLower(override);
        set.indexCache.note = "override:" + ToLower(override);
    }
    return set;
}

std::string StorageProfileSet::describe() const {
    std::ostringstream out;
    const auto line = [&out](const char* role, const IoProfile& profile, const std::string& path) {
        out << role << "=" << StorageClassName(profile.storageClass)
            << "[" << profile.note << "]"
            << " queue=" << profile.readQueueDepth
            << " seqBlockMB=" << (profile.sequentialBlockBytes / (1024 * 1024))
            << " mergeGapKB=" << (profile.mergeGapBytes / 1024)
            << " prefetch=" << profile.prefetchDistance
            << " writeBatchMB=" << (profile.writeBatchBytes / (1024 * 1024))
            << " bgIO=" << profile.backgroundIoPercent << "%"
            << (profile.deviceName.empty() ? "" : (" device=" + profile.deviceName))
            << (profile.busType.empty() ? "" : (" bus=" + profile.busType))
            << " path=" << path << "\n";
    };
    line("source", source, sourcePath);
    line("workspace", workspace, workspacePath);
    line("indexCache", indexCache, indexCachePath);
    return out.str();
}

} // namespace engine
} // namespace seismic