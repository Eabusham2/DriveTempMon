#pragma once
#include "model.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace dtm::sensor_parser {
inline std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}
inline std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline bool plausible(int celsius) { return celsius >= -40 && celsius <= 150; }

// NVMe SMART/Health Information log, LID 02h. Bytes 1-2 are composite Kelvin.
inline int nvme(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || size < 512) return kUnknownTemperature;
    const unsigned kelvin = le16(bytes + 1);
    if (!kelvin || kelvin == 0xffff) return kUnknownTemperature;
    const int celsius = static_cast<int>(kelvin) - 273;
    return plausible(celsius) ? celsius : kUnknownTemperature;
}

// ATA SMART READ DATA is a 512-byte block; attribute raw values are vendor
// defined. Interpret only conventional Celsius attributes 194, then 190.
inline int ata(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || size < 512 || le16(bytes) == 0 || le16(bytes) == 0xffff)
        return kUnknownTemperature;
    unsigned checksum = 0;
    for (std::size_t i = 0; i < 512; ++i) checksum += bytes[i];
    if ((checksum & 0xffu) != 0) return kUnknownTemperature;
    for (const unsigned wanted : {194u, 190u}) {
        for (std::size_t i = 0; i < 30; ++i) {
            const auto* attribute = bytes + 2 + 12 * i;
            if (attribute[0] != wanted) continue;
            const int celsius = attribute[5];
            if (celsius <= 125) return celsius;
        }
    }
    return kUnknownTemperature;
}

// Storage temperature descriptor: fixed 24-byte header, 16-byte sensor records.
// Prefer sensor 0 (composite); otherwise show the hottest available sensor.
inline int storage(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || size < 40) return kUnknownTemperature;
    const auto version = le32(bytes);
    const auto declared = le32(bytes + 4);
    const auto count = le16(bytes + 12);
    if (version < 40 || declared < 40 || declared > size || !count ||
        count > (declared - 24u) / 16u) return kUnknownTemperature;
    int hottest = kUnknownTemperature;
    for (unsigned i = 0; i < count; ++i) {
        const auto* sensor = bytes + 24 + 16 * i;
        const unsigned raw = le16(sensor + 2);
        const int celsius = raw >= 0x8000u ? static_cast<int>(raw) - 65536 : static_cast<int>(raw);
        if (!plausible(celsius)) continue;
        if (le16(sensor) == 0) return celsius;
        if (celsius > hottest) hottest = celsius;
    }
    return hottest;
}

// SPC temperature LOG SENSE page 0Dh, parameter 0000h (current temperature).
inline int scsi(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || size < 4 || (bytes[0] & 0x3fu) != 0x0d || bytes[1] != 0)
        return kUnknownTemperature;
    const std::size_t pageLength = (static_cast<std::size_t>(bytes[2]) << 8) | bytes[3];
    if (pageLength > size - 4) return kUnknownTemperature;
    const std::size_t end = 4 + pageLength;
    for (std::size_t pos = 4; pos < end;) {
        if (end - pos < 4) return kUnknownTemperature;
        const std::size_t length = bytes[pos + 3];
        if (length > end - pos - 4) return kUnknownTemperature;
        const unsigned code = (static_cast<unsigned>(bytes[pos]) << 8) | bytes[pos + 1];
        if (code == 0 && length >= 2) {
            const int celsius = bytes[pos + 5];
            return celsius <= 150 ? celsius : kUnknownTemperature;
        }
        pos += 4 + length;
    }
    return kUnknownTemperature;
}
}
