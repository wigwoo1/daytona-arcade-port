#pragma once

#include <bit>
#include <cstdint>

namespace rt {
// The i960 bus and its video RAM retain little-endian byte storage on every
// host. Converting to or from a native integer uses the same operation.
constexpr uint16_t little_endian_value(uint16_t value) {
    if constexpr (std::endian::native == std::endian::little) return value;
    return uint16_t((value >> 8) | (value << 8));
}
constexpr uint32_t little_endian_value(uint32_t value) {
    if constexpr (std::endian::native == std::endian::little) return value;
    return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
}
static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);
static_assert(little_endian_value(uint32_t(std::endian::native == std::endian::little
                  ? 0x78563412u : 0x12345678u)) == 0x78563412u);
static_assert(little_endian_value(uint16_t(std::endian::native == std::endian::little
                  ? 0x3412u : 0x1234u)) == 0x3412u);
} // namespace rt