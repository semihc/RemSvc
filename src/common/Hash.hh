#pragma once
#ifndef RS_HASH_HH
#define RS_HASH_HH

#include <string>
#include <string_view>

namespace RS {

// CRC-32/ISO-HDLC (same polynomial as zlib) of data,
// returned as 8 lowercase hex digits.
std::string crc32Hex(std::string_view data);

} // namespace RS

#endif // RS_HASH_HH
