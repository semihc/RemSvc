#include "Hash.hh"

#include <array>
#include <cstdint>
#include <cstdio>

namespace RS {

std::string crc32Hex(std::string_view data)
{
    // CRC-32/ISO-HDLC — reflected polynomial 0xEDB88320 (same as zlib crc32).
    // Table is built once via a static local lambda (thread-safe since C++11).
    static const auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data)
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    crc ^= 0xFFFFFFFFu;

    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", crc);
    return buf;
}

} // namespace RS
