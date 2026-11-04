// exr_write.h - minimal OpenEXR writer (scanline, uncompressed, RGB half or float).
// Part of braw2exr. MIT licence (see LICENSE). No dependency.
//
// Writes a valid single-part scanline OpenEXR 2 file: channels B,G,R (EXR
// stores them sorted by name), NO_COMPRESSION, one scanline per block,
// INCREASING_Y. Input: interleaved RGB float, top line first.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace exrw {

inline uint16_t floatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t absx = x & 0x7fffffffu;
    if (absx >= 0x7f800000u) {                       // Inf / NaN
        return (uint16_t)(sign | 0x7c00u | (absx > 0x7f800000u ? 0x200u : 0u));
    }
    if (absx >= 0x477ff000u) {                       // overflow -> Inf (>= 65520)
        return (uint16_t)(sign | 0x7c00u);
    }
    if (absx < 0x38800000u) {                        // subnormal half / zero
        if (absx < 0x33000000u) {
            return (uint16_t)sign;
        }
        const uint32_t e = absx >> 23;
        uint32_t m = (absx & 0x7fffffu) | 0x800000u;
        const int shift = 126 - (int)e;              // 14..24
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u);
        const uint32_t half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) {
            ++h;
        }
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((absx - 0x38000000u) >> 13);       // rebias exponent 127 -> 15
    const uint32_t rem = absx & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) {
        ++h;                                         // round to nearest even (may carry into exponent: fine)
    }
    return (uint16_t)(sign | h);
}

class Buf {
public:
    std::vector<unsigned char> d;
    void u8(uint8_t v) { d.push_back(v); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back((unsigned char)((v >> (8 * i)) & 0xff)); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) d.push_back((unsigned char)((v >> (8 * i)) & 0xff)); }
    void f32(float f) { uint32_t x; std::memcpy(&x, &f, 4); u32(x); }
    void str(const char* s) { while (*s) d.push_back((unsigned char)*s++); d.push_back(0); }
    void attr(const char* name, const char* type, uint32_t size) { str(name); str(type); u32(size); }
};

// pixels: width*height*3 floats (R,G,B interleaved), row 0 = top.
// useHalf: 16-bit half (smaller, enough for comp) or 32-bit float.
// Returns false on I/O error.
inline bool writeRGB(const std::string& path, int width, int height, const float* pixels, bool useHalf,
                     const std::vector<std::pair<std::string, std::string> >& stringAttrs =
                         std::vector<std::pair<std::string, std::string> >())
{
    if (width <= 0 || height <= 0 || !pixels) {
        return false;
    }
    const uint32_t pixelType = useHalf ? 1u : 2u;    // 1 HALF, 2 FLOAT
    const int bytesPerSample = useHalf ? 2 : 4;

    Buf h;
    h.u32(20000630u);                                // magic 76 2f 31 01
    h.u32(2u);                                       // version 2, scanline, no flags

    // channels: B, G, R (sorted). Each: name\0 pixelType(i32) pLinear(u8) reserved(3) xSampling(i32) ySampling(i32)
    const char* chans[3] = { "B", "G", "R" };
    h.attr("channels", "chlist", 3 * (2 + 16) + 1);
    for (int c = 0; c < 3; ++c) {
        h.str(chans[c]);
        h.i32((int32_t)pixelType);
        h.u8(0); h.u8(0); h.u8(0); h.u8(0);
        h.i32(1); h.i32(1);
    }
    h.u8(0);
    h.attr("compression", "compression", 1); h.u8(0);                 // NO_COMPRESSION
    h.attr("dataWindow", "box2i", 16); h.i32(0); h.i32(0); h.i32(width - 1); h.i32(height - 1);
    h.attr("displayWindow", "box2i", 16); h.i32(0); h.i32(0); h.i32(width - 1); h.i32(height - 1);
    h.attr("lineOrder", "lineOrder", 1); h.u8(0);                     // INCREASING_Y
    h.attr("pixelAspectRatio", "float", 4); h.f32(1.f);
    h.attr("screenWindowCenter", "v2f", 8); h.f32(0.f); h.f32(0.f);
    h.attr("screenWindowWidth", "float", 4); h.f32(1.f);
    for (size_t i = 0; i < stringAttrs.size(); ++i) {
        const std::string& v = stringAttrs[i].second;
        h.attr(stringAttrs[i].first.c_str(), "string", (uint32_t)v.size());
        for (size_t k = 0; k < v.size(); ++k) h.u8((uint8_t)v[k]);
    }
    h.u8(0);                                         // end of header

    const uint64_t lineBytes = (uint64_t)width * 3 * bytesPerSample;
    const uint64_t blockBytes = 8 + lineBytes;       // y + size + data
    const uint64_t tableStart = h.d.size();
    const uint64_t dataStart = tableStart + 8ull * (uint64_t)height;
    for (int y = 0; y < height; ++y) {
        h.u64(dataStart + (uint64_t)y * blockBytes);
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite(h.d.data(), 1, h.d.size(), f) == h.d.size();

    std::vector<unsigned char> line((size_t)blockBytes);
    const int order[3] = { 2, 1, 0 };                // B, G, R from interleaved RGB
    for (int y = 0; ok && y < height; ++y) {
        unsigned char* p = line.data();
        const int32_t yy = y;
        const uint32_t sz = (uint32_t)lineBytes;
        for (int i = 0; i < 4; ++i) *p++ = (unsigned char)(((uint32_t)yy >> (8 * i)) & 0xff);
        for (int i = 0; i < 4; ++i) *p++ = (unsigned char)((sz >> (8 * i)) & 0xff);
        const float* row = pixels + (size_t)y * (size_t)width * 3;
        for (int c = 0; c < 3; ++c) {
            const int ch = order[c];
            for (int x = 0; x < width; ++x) {
                const float v = row[(size_t)x * 3 + ch];
                if (useHalf) {
                    const uint16_t hv = floatToHalf(v);
                    *p++ = (unsigned char)(hv & 0xff);
                    *p++ = (unsigned char)(hv >> 8);
                } else {
                    uint32_t xv;
                    std::memcpy(&xv, &v, 4);
                    for (int i = 0; i < 4; ++i) *p++ = (unsigned char)((xv >> (8 * i)) & 0xff);
                }
            }
        }
        ok = std::fwrite(line.data(), 1, line.size(), f) == line.size();
    }
    if (std::fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

} // namespace exrw
