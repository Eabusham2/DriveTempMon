#include "../src/sensor_parsers.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
unsigned checks = 0;
void expect(bool value, const char* description) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
void set16(std::uint8_t* bytes, unsigned value) { bytes[0] = static_cast<std::uint8_t>(value); bytes[1] = static_cast<std::uint8_t>(value >> 8); }
void set32(std::uint8_t* bytes, unsigned value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void checksum(std::array<std::uint8_t, 512>& data) {
    data[511] = 0;
    unsigned sum = 0;
    for (const auto byte : data) sum += byte;
    data[511] = static_cast<std::uint8_t>(0u - sum);
}
}

int main() {
    using namespace dtm;
    using namespace dtm::sensor_parser;
    std::array<std::uint8_t, 512> data{};
    expect(nvme(nullptr, 512) == kUnknownTemperature, "NVMe null");
    expect(nvme(data.data(), 511) == kUnknownTemperature, "NVMe truncated log");
    expect(nvme(data.data(), data.size()) == kUnknownTemperature, "NVMe unreported Kelvin");
    set16(data.data() + 1, 323);
    expect(nvme(data.data(), data.size()) == 50, "NVMe exact 50 Celsius");
    set16(data.data() + 1, 0xffff);
    expect(nvme(data.data(), data.size()) == kUnknownTemperature, "NVMe unavailable marker");
    set16(data.data() + 1, 223);
    expect(nvme(data.data(), data.size()) == kUnknownTemperature, "NVMe implausible negative");
    set16(data.data() + 1, 268);
    expect(nvme(data.data(), data.size()) == -5, "NVMe plausible negative Celsius");

    data.fill(0);
    data[0] = 1;
    data[2] = 190;
    data[7] = 35;
    data[14] = 194;
    data[19] = 52;
    data[20] = 75; // A vendor max/min byte must not be folded into Celsius.
    checksum(data);
    expect(ata(data.data(), data.size()) == 52, "ATA prefer raw 194 over 190");
    expect(ata(data.data(), 511) == kUnknownTemperature, "ATA truncated");
    data[19] = 255;
    checksum(data);
    expect(ata(data.data(), data.size()) == 35, "ATA invalid 194 falls back to 190");
    data[7] = 0;
    checksum(data);
    expect(ata(data.data(), data.size()) == 0, "ATA zero Celsius is valid");
    data[7] = 45;
    expect(ata(data.data(), data.size()) == kUnknownTemperature, "ATA corrupt checksum rejected");
    data.fill(0);
    expect(ata(data.data(), data.size()) == kUnknownTemperature, "ATA zero-filled reply rejected");
    data[0] = 1;
    data[2] = 5; data[7] = 50;
    checksum(data);
    expect(ata(data.data(), data.size()) == kUnknownTemperature, "ATA unrelated attribute not temperature");
    expect(ata(nullptr, 512) == kUnknownTemperature, "ATA null");

    std::array<std::uint8_t, 56> descriptor{};
    set32(descriptor.data(), 40);
    set32(descriptor.data() + 4, static_cast<unsigned>(descriptor.size()));
    set16(descriptor.data() + 12, 2);
    set16(descriptor.data() + 24, 1);
    set16(descriptor.data() + 26, 70);
    set16(descriptor.data() + 40, 0);
    set16(descriptor.data() + 42, 51);
    expect(storage(descriptor.data(), descriptor.size()) == 51, "Windows composite sensor preferred");
    set16(descriptor.data() + 40, 2);
    expect(storage(descriptor.data(), descriptor.size()) == 70, "Windows hottest physical sensor fallback");
    set16(descriptor.data() + 26, 0x8000);
    expect(storage(descriptor.data(), descriptor.size()) == 51, "Windows unknown temperature filtered");
    set16(descriptor.data() + 42, static_cast<unsigned>(65536 - 5));
    expect(storage(descriptor.data(), descriptor.size()) == -5, "Windows signed negative Celsius");
    set16(descriptor.data() + 12, 65535);
    expect(storage(descriptor.data(), descriptor.size()) == kUnknownTemperature, "Windows oversized sensor count");
    set16(descriptor.data() + 12, 2);
    expect(storage(descriptor.data(), 40) == kUnknownTemperature, "Windows truncated descriptor");
    set32(descriptor.data() + 4, 0xffffffffu);
    expect(storage(descriptor.data(), descriptor.size()) == kUnknownTemperature, "Windows huge claimed size");
    expect(storage(nullptr, 56) == kUnknownTemperature, "Windows null");

    std::vector<std::uint8_t> log{0x0d, 0, 0, 12, 0, 1, 0, 2, 0, 65, 0, 0, 0, 2, 0, 48};
    expect(scsi(log.data(), log.size()) == 48, "SCSI current not reference temperature");
    log[15] = 0xff;
    expect(scsi(log.data(), log.size()) == kUnknownTemperature, "SCSI unavailable temperature");
    log[15] = 60;
    expect(scsi(log.data(), log.size() - 1) == kUnknownTemperature, "SCSI truncated log");
    log[7] = 255;
    expect(scsi(log.data(), log.size()) == kUnknownTemperature, "SCSI oversized parameter");
    log[7] = 2;
    log[0] = 0x2f;
    expect(scsi(log.data(), log.size()) == kUnknownTemperature, "SCSI wrong log page");
    expect(scsi(nullptr, 16) == kUnknownTemperature, "SCSI null");
    std::cout << checks << " sensor parser checks passed\n";
}
