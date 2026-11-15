#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <ntddscsi.h>
#include "sensors.hpp"
#include "sensor_parsers.hpp"
#include "policy.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace dtm {
namespace {
constexpr GUID kDiskInterface = {0x53f56307, 0xb6bf, 0x11d0,
                                {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
constexpr DWORD kCommandTimeoutSeconds = 2;
constexpr ULONGLONG kUnsupportedRetryMs = 300000;
constexpr ULONGLONG kReadFailureRetryMs = 30000;
constexpr DWORD kMaximumDescriptorBytes = 65536;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = other.value; other.value = INVALID_HANDLE_VALUE; }
        return *this;
    }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (*this) CloseHandle(value);
        value = h;
    }
};

enum class Method { Untried, StorageDevice, StorageAdapter, NvmeDevice, NvmeAdapter,
                    Ata, LegacySmart, Sat16, Sat12, Scsi, Unsupported };
struct Endpoint {
    Drive drive;
    Handle handle;
    bool readWrite = false;
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    Method method = Method::Untried;
    ULONGLONG nextProbe = 0;
    unsigned failures = 0;
};

std::wstring trim(std::wstring text) {
    for (auto& c : text) if (c < L' ') c = L' ';
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(L" \t\r\n");
    text = text.substr(first, last - first + 1);
    auto end = std::unique(text.begin(), text.end(), [](wchar_t a, wchar_t b) {
        return a == L' ' && b == L' ';
    });
    text.erase(end, text.end());
    return text;
}

std::wstring descriptorString(const BYTE* data, std::size_t length, DWORD offset) {
    if (!offset || offset >= length) return {};
    const char* value = reinterpret_cast<const char*>(data + offset);
    const auto available = length - offset;
    const void* terminator = std::memchr(value, 0, available);
    if (!terminator) return {};
    const auto count = static_cast<int>(static_cast<const char*>(terminator) - value);
    if (!count) return {};
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, count, nullptr, 0);
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (!needed) { page = CP_ACP; flags = 0; needed = MultiByteToWideChar(page, flags, value, count, nullptr, 0); }
    if (!needed) return {};
    std::wstring output(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(page, flags, value, count, output.data(), needed);
    return trim(std::move(output));
}

std::wstring propertyString(HDEVINFO info, SP_DEVINFO_DATA& device, DWORD property) {
    std::array<wchar_t, 512> text{};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(info, &device, property, &type,
        reinterpret_cast<BYTE*>(text.data()), static_cast<DWORD>(sizeof(text)), nullptr) ||
        (type != REG_SZ && type != REG_MULTI_SZ)) return {};
    text.back() = 0;
    return trim(text.data());
}

std::wstring stableId(const std::wstring& value) {
    // Keep INI keys short and free of punctuation; serial/model remain local.
    std::uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : value) {
        const auto v = static_cast<std::uint32_t>(c);
        hash ^= v & 0xffu; hash *= 1099511628211ull;
        hash ^= (v >> 8) & 0xffu; hash *= 1099511628211ull;
    }
    return L"drive-" + std::to_wstring(hash);
}

bool query(HANDLE handle, STORAGE_PROPERTY_ID property, void* out, DWORD size, DWORD& returned) {
    STORAGE_PROPERTY_QUERY input{};
    input.PropertyId = property;
    input.QueryType = PropertyStandardQuery;
    return DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &input, sizeof(input),
                           out, size, &returned, nullptr) != FALSE;
}

std::wstring readIdentity(Endpoint& endpoint) {
    STORAGE_PROPERTY_QUERY input{};
    input.PropertyId = StorageDeviceProperty;
    input.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD returned = 0;
    if (!DeviceIoControl(endpoint.handle.value, IOCTL_STORAGE_QUERY_PROPERTY,
        &input, sizeof(input), &header, sizeof(header), &returned, nullptr) ||
        returned < sizeof(header) || header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
        header.Size > kMaximumDescriptorBytes) return {};
    std::vector<BYTE> buffer(header.Size);
    if (!DeviceIoControl(endpoint.handle.value, IOCTL_STORAGE_QUERY_PROPERTY,
        &input, sizeof(input), buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
        returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (descriptor->Size > returned || descriptor->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};
    const std::wstring vendor = descriptorString(buffer.data(), descriptor->Size, descriptor->VendorIdOffset);
    const std::wstring product = descriptorString(buffer.data(), descriptor->Size, descriptor->ProductIdOffset);
    if (!product.empty()) endpoint.drive.name = vendor.empty() ? product : trim(vendor + L" " + product);
    endpoint.bus = descriptor->BusType;
    return descriptorString(buffer.data(), descriptor->Size, descriptor->SerialNumberOffset);
}

int storageTemperature(HANDLE handle, STORAGE_PROPERTY_ID property) {
    alignas(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) std::array<BYTE, 4096> data{};
    DWORD returned = 0;
    if (!query(handle, property, data.data(), static_cast<DWORD>(data.size()), returned))
        return kUnknownTemperature;
    return sensor_parser::storage(data.data(), (std::min)(static_cast<std::size_t>(returned), data.size()));
}

int nvmeTemperature(HANDLE handle, STORAGE_PROPERTY_ID property) {
    constexpr std::size_t base = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
    alignas(STORAGE_PROTOCOL_DATA_DESCRIPTOR)
        std::array<BYTE, base + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512> data{};
    auto* request = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(data.data());
    request->PropertyId = property;
    request->QueryType = PropertyStandardQuery;
    auto* protocol = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(data.data() + base);
    protocol->ProtocolType = ProtocolTypeNvme;
    protocol->DataType = NVMeDataTypeLogPage;
    protocol->ProtocolDataRequestValue = 2; // SMART/Health Information log only.
    protocol->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    protocol->ProtocolDataLength = 512;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, data.data(),
        static_cast<DWORD>(data.size()), data.data(), static_cast<DWORD>(data.size()), &returned, nullptr) ||
        returned < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) || returned > data.size()) return kUnknownTemperature;
    const auto* descriptor = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(data.data());
    if (descriptor->Version < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) ||
        descriptor->Size < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) return kUnknownTemperature;
    const auto& result = descriptor->ProtocolSpecificData;
    const std::size_t protocolBase = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    if (result.ProtocolType != ProtocolTypeNvme || result.DataType != NVMeDataTypeLogPage ||
        result.ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) ||
        result.ProtocolDataOffset > returned - protocolBase || result.ProtocolDataLength < 512)
        return kUnknownTemperature;
    const std::size_t offset = protocolBase + result.ProtocolDataOffset;
    if (result.ProtocolDataLength > returned - offset) return kUnknownTemperature;
    return sensor_parser::nvme(data.data() + offset, result.ProtocolDataLength);
}

int ataTemperature(HANDLE handle) {
    struct Packet { ATA_PASS_THROUGH_EX command; ULONG alignment; BYTE data[512]; } packet{};
    packet.command.Length = sizeof(ATA_PASS_THROUGH_EX);
    packet.command.AtaFlags = ATA_FLAGS_DATA_IN;
    packet.command.DataTransferLength = sizeof(packet.data);
    packet.command.TimeOutValue = kCommandTimeoutSeconds;
    packet.command.DataBufferOffset = offsetof(Packet, data);
    packet.command.CurrentTaskFile[0] = 0xd0; // SMART READ DATA, never ENABLE/WRITE.
    packet.command.CurrentTaskFile[1] = 1;
    packet.command.CurrentTaskFile[2] = 1;
    packet.command.CurrentTaskFile[3] = 0x4f;
    packet.command.CurrentTaskFile[4] = 0xc2;
    packet.command.CurrentTaskFile[6] = 0xb0;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_ATA_PASS_THROUGH, &packet, sizeof(ATA_PASS_THROUGH_EX),
        &packet, sizeof(packet), &returned, nullptr) || returned < offsetof(Packet, data) + 512 ||
        packet.command.DataTransferLength != 512 || (packet.command.CurrentTaskFile[6] & 0x21) != 0)
        return kUnknownTemperature;
    return sensor_parser::ata(packet.data, sizeof(packet.data));
}

int legacyTemperature(HANDLE handle) {
    SENDCMDINPARAMS input{};
    input.cBufferSize = 512;
    input.irDriveRegs.bFeaturesReg = 0xd0;
    input.irDriveRegs.bSectorCountReg = 1;
    input.irDriveRegs.bSectorNumberReg = 1;
    input.irDriveRegs.bCylLowReg = 0x4f;
    input.irDriveRegs.bCylHighReg = 0xc2;
    input.irDriveRegs.bCommandReg = 0xb0;
    alignas(SENDCMDOUTPARAMS) std::array<BYTE, offsetof(SENDCMDOUTPARAMS, bBuffer) + 512> data{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, SMART_RCV_DRIVE_DATA, &input, sizeof(input) - 1,
        data.data(), static_cast<DWORD>(data.size()), &returned, nullptr) || returned < data.size())
        return kUnknownTemperature;
    const auto* output = reinterpret_cast<const SENDCMDOUTPARAMS*>(data.data());
    if (output->DriverStatus.bDriverError != 0 || output->cBufferSize < 512)
        return kUnknownTemperature;
    return sensor_parser::ata(output->bBuffer, 512);
}

struct ScsiPacket { SCSI_PASS_THROUGH command; ULONG alignment; BYTE sense[32]; BYTE data[512]; };

bool scsiRead(HANDLE handle, ScsiPacket& packet) {
    packet.command.Length = sizeof(SCSI_PASS_THROUGH);
    packet.command.SenseInfoLength = sizeof(packet.sense);
    packet.command.DataIn = SCSI_IOCTL_DATA_IN;
    packet.command.DataTransferLength = sizeof(packet.data);
    packet.command.TimeOutValue = kCommandTimeoutSeconds;
    packet.command.DataBufferOffset = offsetof(ScsiPacket, data);
    packet.command.SenseInfoOffset = offsetof(ScsiPacket, sense);
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH, &packet,
        static_cast<DWORD>(offsetof(ScsiPacket, data)), &packet, sizeof(packet), &returned, nullptr) ||
        packet.command.ScsiStatus != 0 || returned < offsetof(ScsiPacket, data) ||
        packet.command.DataTransferLength > sizeof(packet.data) ||
        packet.command.DataTransferLength > returned - offsetof(ScsiPacket, data)) return false;
    return true;
}

int satTemperature(HANDLE handle, bool sixteenByteCdb) {
    ScsiPacket packet{};
    auto* cdb = packet.command.Cdb;
    if (sixteenByteCdb) {
        packet.command.CdbLength = 16;
        cdb[0] = 0x85; // ATA PASS THROUGH (16).
        cdb[1] = 4 << 1; // PIO Data-In protocol.
        cdb[2] = 0x0e; // T_DIR=read, BYT_BLOK=block, T_LENGTH=sector count; CK_COND=0.
        cdb[4] = 0xd0;
        cdb[6] = 1;
        cdb[8] = 1;
        cdb[10] = 0x4f;
        cdb[12] = 0xc2;
        cdb[14] = 0xb0;
    } else {
        packet.command.CdbLength = 12;
        cdb[0] = 0xa1; // ATA PASS THROUGH (12).
        cdb[1] = 4 << 1;
        cdb[2] = 0x0e;
        cdb[3] = 0xd0;
        cdb[4] = 1;
        cdb[5] = 1;
        cdb[6] = 0x4f;
        cdb[7] = 0xc2;
        cdb[9] = 0xb0;
    }
    if (!scsiRead(handle, packet)) return kUnknownTemperature;
    return sensor_parser::ata(packet.data, packet.command.DataTransferLength);
}

int scsiTemperature(HANDLE handle) {
    ScsiPacket packet{};
    packet.command.CdbLength = 10;
    packet.command.Cdb[0] = 0x4d; // LOG SENSE (10), read only.
    packet.command.Cdb[2] = 0x4d; // PC=current cumulative, page 0Dh.
    packet.command.Cdb[7] = 2; // Allocation length: 512 bytes.
    if (!scsiRead(handle, packet)) return kUnknownTemperature;
    return sensor_parser::scsi(packet.data, packet.command.DataTransferLength);
}

int read(Endpoint& endpoint, Method method) {
    switch (method) {
    case Method::StorageDevice: return storageTemperature(endpoint.handle.value, StorageDeviceTemperatureProperty);
    case Method::StorageAdapter: return storageTemperature(endpoint.handle.value, StorageAdapterTemperatureProperty);
    case Method::NvmeDevice: return nvmeTemperature(endpoint.handle.value, StorageDeviceProtocolSpecificProperty);
    case Method::NvmeAdapter: return nvmeTemperature(endpoint.handle.value, StorageAdapterProtocolSpecificProperty);
    case Method::Ata: return endpoint.readWrite ? ataTemperature(endpoint.handle.value) : kUnknownTemperature;
    case Method::LegacySmart: return endpoint.readWrite ? legacyTemperature(endpoint.handle.value) : kUnknownTemperature;
    case Method::Sat16: return endpoint.readWrite ? satTemperature(endpoint.handle.value, true) : kUnknownTemperature;
    case Method::Sat12: return endpoint.readWrite ? satTemperature(endpoint.handle.value, false) : kUnknownTemperature;
    case Method::Scsi: return endpoint.readWrite ? scsiTemperature(endpoint.handle.value) : kUnknownTemperature;
    default: return kUnknownTemperature;
    }
}

const wchar_t* methodName(Method method) {
    switch (method) {
    case Method::StorageDevice: case Method::StorageAdapter: return L"Windows temperature query";
    case Method::NvmeDevice: case Method::NvmeAdapter: return L"NVMe SMART temperature";
    case Method::Ata: return L"ATA SMART temperature";
    case Method::LegacySmart: return L"SMART temperature";
    case Method::Sat16: case Method::Sat12: return L"USB/SAT SMART temperature";
    case Method::Scsi: return L"SCSI temperature log";
    default: return L"Temperature unavailable";
    }
}

void pollEndpoint(Endpoint& endpoint) {
    auto& drive = endpoint.drive;
    drive.sleeping = false;
    drive.celsius = kUnknownTemperature;
    if (!endpoint.handle) { drive.status = L"Drive unavailable or access denied"; return; }
    BOOL powered = TRUE;
    if (GetDevicePowerState(endpoint.handle.value, &powered) && !powered) {
        drive.sleeping = true;
        drive.status = L"Sleeping - temperature not polled";
        return;
    }
    const auto now = GetTickCount64();
    if ((endpoint.method == Method::Unsupported || endpoint.method == Method::Untried) && now < endpoint.nextProbe) {
        drive.status = endpoint.method == Method::Untried ? L"Temperature read failed - retrying" :
            endpoint.readWrite ? L"Temperature unavailable from this drive/controller" :
                                 L"Temperature unavailable - administrator access may be required";
        return;
    }
    int celsius = kUnknownTemperature;
    if (endpoint.method != Method::Untried && endpoint.method != Method::Unsupported) {
        celsius = read(endpoint, endpoint.method);
        if (celsius == kUnknownTemperature) {
            drive.status = L"Temperature read failed - retrying";
            if (++endpoint.failures >= 3) {
                endpoint.method = Method::Untried;
                endpoint.nextProbe = now + kReadFailureRetryMs;
            }
            return;
        }
    } else {
        const std::array<Method, 4> nvmeMethods{Method::NvmeDevice, Method::NvmeAdapter, Method::StorageDevice, Method::StorageAdapter};
        const std::array<Method, 6> otherMethods{Method::StorageDevice, Method::Ata, Method::LegacySmart, Method::Sat16, Method::Sat12, Method::Scsi};
        auto tryMethod = [&](Method method) {
            celsius = read(endpoint, method);
            if (celsius == kUnknownTemperature) return false;
            endpoint.method = method;
            return true;
        };
        if (endpoint.bus == BusTypeNvme) {
            for (const auto method : nvmeMethods) if (tryMethod(method)) break;
        } else {
            for (const auto method : otherMethods) if (tryMethod(method)) break;
        }
        if (celsius == kUnknownTemperature) {
            endpoint.method = Method::Unsupported;
            endpoint.nextProbe = now + kUnsupportedRetryMs;
            drive.status = endpoint.readWrite ? L"Temperature unavailable from this drive/controller"
                                             : L"Temperature unavailable - administrator access may be required";
            return;
        }
    }
    drive.celsius = celsius;
    drive.sampledAtMs = now;
    drive.status = methodName(endpoint.method);
    endpoint.failures = 0;
}
}

struct Sensors::Impl {
    std::vector<Endpoint> endpoints;
    bool enumerated = false;

    void discover() {
        const HDEVINFO devices = SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr,
                                                     DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) return;
        std::vector<Endpoint> discovered;
        std::set<DWORD> seenNumbers;
        std::map<std::wstring, unsigned> idCounts;
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kDiskInterface, index, &iface)) break;
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
            if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed > kMaximumDescriptorBytes) continue;
            std::vector<BYTE> bytes(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, &device)) continue;
            Endpoint endpoint;
            auto& drive = endpoint.drive;
            drive.path = detail->DevicePath;
            drive.number = index;
            drive.name = propertyString(devices, device, SPDRP_FRIENDLYNAME);
            if (drive.name.empty()) drive.name = propertyString(devices, device, SPDRP_DEVICEDESC);
            std::array<wchar_t, 1024> instance{};
            SetupDiGetDeviceInstanceIdW(devices, &device, instance.data(), static_cast<DWORD>(instance.size()), nullptr);
            instance.back() = 0;
            endpoint.handle.reset(CreateFileW(drive.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            endpoint.readWrite = static_cast<bool>(endpoint.handle);
            if (!endpoint.handle) endpoint.handle.reset(CreateFileW(drive.path.c_str(), 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            std::wstring serial;
            if (endpoint.handle) {
                STORAGE_DEVICE_NUMBER number{};
                DWORD returned = 0;
                if (DeviceIoControl(endpoint.handle.value, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                    &number, sizeof(number), &returned, nullptr) && returned >= sizeof(number)) {
                    if (number.DeviceType != FILE_DEVICE_DISK || !seenNumbers.insert(number.DeviceNumber).second) continue;
                    drive.number = number.DeviceNumber;
                }
                serial = readIdentity(endpoint);
                if (endpoint.bus == BusTypeNvme) {
                    drive.kind = DriveKind::SSD;
                } else {
                    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
                    if (query(endpoint.handle.value, StorageDeviceSeekPenaltyProperty, &penalty, sizeof(penalty), returned) &&
                        returned >= sizeof(penalty) && penalty.Size >= sizeof(penalty))
                        drive.kind = penalty.IncursSeekPenalty ? DriveKind::HDD : DriveKind::SSD;
                }
            }
            if (drive.name.empty()) drive.name = L"Physical drive " + std::to_wstring(drive.number);
            drive.shortName = shortDriveName(drive.name, drive.number);
            const auto identity = serial.empty() ? (instance[0] ? std::wstring(instance.data()) : drive.path)
                                               : std::to_wstring(endpoint.bus) + L"|" + drive.name + L"|" + serial;
            drive.id = stableId(identity);
            ++idCounts[drive.id];
            discovered.push_back(std::move(endpoint));
        }
        SetupDiDestroyDeviceInfoList(devices);
        for (auto& endpoint : discovered) {
            auto& drive = endpoint.drive;
            // Some USB bridges expose the same placeholder serial for every disk.
            // Disambiguate every duplicate, so enumeration order cannot swap IDs.
            if (idCounts[drive.id] > 1) drive.id = stableId(drive.id + L"|" + drive.path);
            const auto existing = std::find_if(endpoints.begin(), endpoints.end(), [&](const Endpoint& old) {
                return old.drive.id == drive.id;
            });
            if (existing != endpoints.end()) {
                drive.celsius = existing->drive.celsius;
                drive.sampledAtMs = existing->drive.sampledAtMs;
                drive.sleeping = existing->drive.sleeping;
                drive.status = existing->drive.status;
                endpoint.method = existing->method;
                endpoint.failures = existing->failures;
                endpoint.nextProbe = existing->nextProbe;
            }
        }
        std::sort(discovered.begin(), discovered.end(), [](const Endpoint& a, const Endpoint& b) {
            return a.drive.number < b.drive.number;
        });
        endpoints = std::move(discovered);
        enumerated = true;
    }
};

Sensors::Sensors() : impl_(std::make_unique<Impl>()) {}
Sensors::~Sensors() = default;

std::vector<Drive> Sensors::poll(bool rediscover, const std::vector<std::wstring>& onlyIds) {
    if (!impl_->enumerated || rediscover) impl_->discover();
    std::vector<Drive> result;
    result.reserve(impl_->endpoints.size());
    for (auto& endpoint : impl_->endpoints) {
        if (onlyIds.empty() || std::find(onlyIds.begin(), onlyIds.end(), endpoint.drive.id) != onlyIds.end())
            pollEndpoint(endpoint);
        result.push_back(endpoint.drive);
    }
    return result;
}
}
