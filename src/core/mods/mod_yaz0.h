// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace Core::Mods {
// Bounded decoder for Nintendo Yaz0-wrapped UI archives. Reject malformed backreferences.
inline bool DecodeYaz0(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit) {
    out.clear();
    if (in.size() < 16 || in[0] != 'Y' || in[1] != 'a' || in[2] != 'z' || in[3] != '0')
        return false;
    size_t size = (size_t(in[4]) << 24) | (size_t(in[5]) << 16) | (size_t(in[6]) << 8) | in[7];
    if (!size || size > limit)
        return false;
    out.reserve(size);
    size_t p = 16;
    while (out.size() < size) {
        if (p >= in.size()) {
            out.clear();
            return false;
        }
        uint8_t flags = in[p++];
        for (unsigned bit = 0; bit < 8 && out.size() < size; bit++, flags <<= 1) {
            if (flags & 0x80) {
                if (p >= in.size()) {
                    out.clear();
                    return false;
                }
                out.push_back(in[p++]);
            } else {
                if (in.size() - p < 2) {
                    out.clear();
                    return false;
                }
                uint8_t a = in[p++], b = in[p++];
                size_t distance = ((a & 15) << 8) + b + 1;
                size_t length = a >> 4;
                if (!length) {
                    if (p >= in.size()) {
                        out.clear();
                        return false;
                    }
                    length = in[p++] + 0x12;
                } else
                    length += 2;
                if (distance > out.size() || length > size - out.size()) {
                    out.clear();
                    return false;
                }
                for (size_t i = 0; i < length; i++)
                    out.push_back(out[out.size() - distance]);
            }
        }
    }
    return true;
}
} // namespace Core::Mods
