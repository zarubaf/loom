// SPDX-License-Identifier: Apache-2.0
// Loom SHA-256 utilities — thin wrapper around PicoSHA2
//
// Provides convenience functions for computing SHA-256 hashes and
// extracting 8 x 32-bit words for the emu_ctrl design hash registers.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "picosha2.h"

namespace loom {

// Compute SHA-256, return 32-byte digest
inline std::array<uint8_t, 32> sha256(const std::string& data) {
    std::array<uint8_t, 32> hash;
    picosha2::hash256(data.begin(), data.end(), hash.begin(), hash.end());
    return hash;
}

// Convert 32-byte digest to 64-char hex string
inline std::string sha256_hex(const std::array<uint8_t, 32>& hash) {
    return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

// Extract 8 x 32-bit words from SHA-256 hash (for emu_ctrl registers)
// Word 0 = hash[31:0] (least significant), Word 7 = hash[255:224] (most significant)
inline std::array<uint32_t, 8> sha256_words(const std::array<uint8_t, 32>& hash) {
    std::array<uint32_t, 8> words;
    for (int i = 0; i < 8; i++) {
        // Word 0 = bytes [28..31], Word 7 = bytes [0..3]
        int base = (7 - i) * 4;
        words[i] = (static_cast<uint32_t>(hash[base + 0]) << 24)
                 | (static_cast<uint32_t>(hash[base + 1]) << 16)
                 | (static_cast<uint32_t>(hash[base + 2]) << 8)
                 | (static_cast<uint32_t>(hash[base + 3]));
    }
    return words;
}

} // namespace loom
