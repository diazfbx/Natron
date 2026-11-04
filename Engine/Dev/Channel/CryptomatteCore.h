/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_CRYPTOMATTECORE_H
#define NATRON_ENGINE_CRYPTOMATTECORE_H

/*
 * Cryptomatte helpers with no Natron dependency (unit-testable on their own).
 *
 * Implements the parts of the Cryptomatte 1.2 specification
 * (https://github.com/Psyop/Cryptomatte/blob/master/specification/cryptomatte_specification.pdf)
 * a reader needs:
 *  - name -> ID: MurmurHash3_x86_32 (seed 0), bits remapped so the result is a
 *    finite, normalised float (exponent clamped to [1, 254]);
 *  - the per-file metadata "cryptomatte/<key>/{name,hash,conversion,manifest,
 *    manif_file}", read straight from the EXR header (string attributes only);
 *  - the manifest, a flat JSON object {"name": "hex id", ...};
 *  - the matte list syntax used by Nuke's Cryptomatte: comma-separated names,
 *    optionally double-quoted (\" and \\ escapes), "<float>" for raw IDs with
 *    no manifest name, and '*' / '?' wildcards resolved against the manifest.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace CryptomatteCore {

// ---------------------------------------------------------------- hashing

inline uint32_t murmur3_32(const std::string& key, uint32_t seed = 0)
{
    const uint8_t* data = (const uint8_t*)key.data();
    const int len = (int)key.size();
    const int nblocks = len / 4;
    uint32_t h1 = seed;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;

    for (int i = 0; i < nblocks; ++i) {
        uint32_t k1;
        std::memcpy(&k1, data + i * 4, 4);      // little-endian hosts (x86/ARM)
        k1 *= c1; k1 = (k1 << 15) | (k1 >> 17); k1 *= c2;
        h1 ^= k1; h1 = (h1 << 13) | (h1 >> 19); h1 = h1 * 5u + 0xe6546b64u;
    }
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= (uint32_t)tail[2] << 16; // fallthrough
    case 2: k1 ^= (uint32_t)tail[1] << 8;  // fallthrough
    case 1: k1 ^= (uint32_t)tail[0];
        k1 *= c1; k1 = (k1 << 15) | (k1 >> 17); k1 *= c2; h1 ^= k1;
    }
    h1 ^= (uint32_t)len;
    h1 ^= h1 >> 16; h1 *= 0x85ebca6bu; h1 ^= h1 >> 13; h1 *= 0xc2b2ae35u; h1 ^= h1 >> 16;
    return h1;
}

// Spec: keep the hash as float bits, but clamp the exponent so the value is
// never a denormal, an infinity or a NaN.
inline uint32_t hashToIdBits(uint32_t hash)
{
    uint32_t mantissa = hash & ((1u << 23) - 1);
    uint32_t exp = (hash >> 23) & 0xffu;
    exp = std::max<uint32_t>(exp, 1u);
    exp = std::min<uint32_t>(exp, 254u);
    return (hash & 0x80000000u) | (exp << 23) | mantissa;
}

inline uint32_t nameToIdBits(const std::string& name) { return hashToIdBits(murmur3_32(name)); }

inline float bitsToFloat(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t floatToBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// "<1.23456e-05>" form Nuke writes for IDs that have no manifest name.
inline std::string idToken(uint32_t bits)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "<%.9g>", (double)bitsToFloat(bits));
    return buf;
}

// Deterministic preview colour of an ID (well spread, mid-bright).
inline void idToColor(uint32_t bits, float rgb[3])
{
    uint32_t h = bits;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    rgb[0] = 0.15f + 0.85f * (float)((h      ) & 0xff) / 255.f;
    rgb[1] = 0.15f + 0.85f * (float)((h >>  8) & 0xff) / 255.f;
    rgb[2] = 0.15f + 0.85f * (float)((h >> 16) & 0xff) / 255.f;
}

// ---------------------------------------------------------------- manifest

// Parse a JSON string token starting at s[i] == '"'. Returns false on error.
inline bool jsonString(const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"') return false;
    ++i; out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (i >= s.size()) return false;
        char e = s[i++];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            if (i + 4 > s.size()) return false;
            unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                unsigned lo = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
            }
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            break;
        }
        default: out += e; break;   // \" \\ \/
        }
    }
    return false;
}

// Manifest JSON: {"name": "0a1b2c3d", ...} -> (name, id bits). Entries whose
// value is not a hex string are skipped. Returns false if not a JSON object.
inline bool parseManifest(const std::string& json, std::vector<std::pair<std::string, uint32_t> >* out)
{
    out->clear();
    size_t i = 0;
    auto ws = [&]() { while (i < json.size() && std::isspace((unsigned char)json[i])) ++i; };
    ws();
    if (i >= json.size() || json[i] != '{') return false;
    ++i;
    for (;;) {
        ws();
        if (i >= json.size()) return false;
        if (json[i] == '}') return true;
        std::string name, value;
        if (!jsonString(json, i, name)) return false;
        ws();
        if (i >= json.size() || json[i] != ':') return false;
        ++i; ws();
        if (i < json.size() && json[i] == '"') {
            if (!jsonString(json, i, value)) return false;
            char* end = nullptr;
            unsigned long v = std::strtoul(value.c_str(), &end, 16);
            if (end && *end == '\0' && !value.empty()) out->push_back(std::make_pair(name, (uint32_t)v));
        } else {
            while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;   // skip a non-string value
        }
        ws();
        if (i < json.size() && json[i] == ',') { ++i; continue; }
        if (i < json.size() && json[i] == '}') return true;
        return false;
    }
}

// ---------------------------------------------------------------- EXR header

// String attributes of every part header of an OpenEXR file (multi-part files
// list one header per part). Non-string attributes are skipped.
inline bool readExrStringAttributes(const std::string& path, std::map<std::string, std::string>* out)
{
    out->clear();
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    unsigned char magic[8];
    if (!f.read((char*)magic, 8)) return false;
    if (!(magic[0] == 0x76 && magic[1] == 0x2f && magic[2] == 0x31 && magic[3] == 0x01)) return false;
    const bool multipart = (magic[5] & 0x10) != 0;
    auto readCStr = [&](std::string& s) -> bool {
        s.clear();
        char c;
        while (f.get(c)) {
            if (c == '\0') return true;
            s += c;
            if (s.size() > 4096) return false;
        }
        return false;
    };
    for (int part = 0; part < 4096; ++part) {
        std::string name;
        if (!readCStr(name)) return false;
        if (name.empty()) {
            if (!multipart || part > 0 /* empty header = end of header list */) break;
            break;
        }
        bool anyAttr = true;
        while (anyAttr) {
            std::string type;
            if (!readCStr(type)) return false;
            uint32_t size;
            if (!f.read((char*)&size, 4)) return false;
            if (size > (1u << 28)) return false;
            if (type == "string") {
                std::string v(size, '\0');
                if (size && !f.read(&v[0], size)) return false;
                if (!out->count(name)) (*out)[name] = v;
            } else {
                f.seekg(size, std::ios::cur);
                if (!f) return false;
            }
            if (!readCStr(name)) return false;
            if (name.empty()) anyAttr = false;      // end of this part's header
        }
        if (!multipart) break;
        // multipart: the next byte either starts the next header or is the
        // terminating empty header (a single null byte).
        int next = f.peek();
        if (next == 0) break;
    }
    return true;
}

struct CryptoLayerInfo
{
    std::string key;          // 7-hex-digit metadata key
    std::string name;         // layer name, e.g. "CryptoObject"
    std::string hash;         // "MurmurHash3_32"
    std::string conversion;   // "uint32_to_float32"
    std::string manifest;     // JSON (inline, or loaded from manif_file)
    std::string manifFile;    // sidecar manifest path, relative to the EXR
};

inline std::string dirOf(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

// Collect the "cryptomatte/<key>/..." entries of a header.
inline std::vector<CryptoLayerInfo> cryptoLayersFromHeader(const std::map<std::string, std::string>& attrs,
                                                           const std::string& exrPath)
{
    std::map<std::string, CryptoLayerInfo> byKey;
    const std::string prefix = "cryptomatte/";
    for (std::map<std::string, std::string>::const_iterator it = attrs.begin(); it != attrs.end(); ++it) {
        const std::string& k = it->first;
        if (k.compare(0, prefix.size(), prefix) != 0) continue;
        size_t slash = k.find('/', prefix.size());
        if (slash == std::string::npos) continue;
        const std::string key = k.substr(prefix.size(), slash - prefix.size());
        const std::string field = k.substr(slash + 1);
        CryptoLayerInfo& L = byKey[key];
        L.key = key;
        if (field == "name") L.name = it->second;
        else if (field == "hash") L.hash = it->second;
        else if (field == "conversion") L.conversion = it->second;
        else if (field == "manifest") L.manifest = it->second;
        else if (field == "manif_file") L.manifFile = it->second;
    }
    std::vector<CryptoLayerInfo> out;
    for (std::map<std::string, CryptoLayerInfo>::iterator it = byKey.begin(); it != byKey.end(); ++it) {
        CryptoLayerInfo& L = it->second;
        if (L.manifest.empty() && !L.manifFile.empty()) {
            std::ifstream mf((dirOf(exrPath) + L.manifFile).c_str(), std::ios::binary);
            if (mf) { std::stringstream ss; ss << mf.rdbuf(); L.manifest = ss.str(); }
        }
        if (!L.name.empty()) out.push_back(L);
    }
    return out;
}

// ---------------------------------------------------------------- matte list

// Split a Nuke-style matte list into entries (quotes and escapes removed).
inline std::vector<std::string> parseMatteList(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false, quoted = false;
    auto flush = [&]() {
        std::string t = cur;
        if (!quoted) {   // trim unquoted entries
            size_t a = t.find_first_not_of(" \t\r\n");
            size_t b = t.find_last_not_of(" \t\r\n");
            t = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
        }
        if (!t.empty()) out.push_back(t);
        cur.clear(); quoted = false;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inQuotes) {
            if (c == '\\' && i + 1 < s.size()) { cur += s[++i]; continue; }
            if (c == '"') { inQuotes = false; continue; }
            cur += c;
        } else if (c == '"') {
            // drop whitespace before an opening quote
            size_t a = cur.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) cur.clear();
            inQuotes = true; quoted = true;
        } else if (c == ',' || c == '\n') {
            flush();
        } else if (!(quoted && (c == ' ' || c == '\t'))) {
            cur += c;
        }
    }
    flush();
    return out;
}

inline bool needsQuotes(const std::string& n)
{
    if (n.empty()) return true;
    if (n.find_first_of(",\"\\\n") != std::string::npos) return true;
    return n.front() == ' ' || n.back() == ' ' || n.front() == '<';
}

inline std::string formatMatteList(const std::vector<std::string>& entries)
{
    std::string out;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) out += "\n";   // one entry per line (the parser also takes commas)
        const std::string& e = entries[i];
        const bool isId = e.size() > 2 && e.front() == '<' && e.back() == '>';
        if (!isId && needsQuotes(e)) {
            out += '"';
            for (char c : e) { if (c == '"' || c == '\\') out += '\\'; out += c; }
            out += '"';
        } else {
            out += e;
        }
    }
    return out;
}

inline bool wildcardMatch(const char* pat, const char* str)
{
    // iterative glob: '*' any sequence, '?' one character
    const char* star = nullptr; const char* ss = str;
    while (*str) {
        if (*pat == '?' || *pat == *str) { ++pat; ++str; continue; }
        if (*pat == '*') { star = pat++; ss = str; continue; }
        if (star) { pat = star + 1; str = ++ss; continue; }
        return false;
    }
    while (*pat == '*') ++pat;
    return *pat == '\0';
}

// Resolve matte-list entries to ID bits. Names resolve through the manifest
// when present, else by hashing the name (same result for a spec-compliant
// renderer); "<float>" entries are raw IDs; wildcard entries match manifest
// names. Output is sorted and unique.
inline std::vector<uint32_t> resolveMatteIds(const std::vector<std::string>& entries,
                                             const std::vector<std::pair<std::string, uint32_t> >& manifest)
{
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& e = entries[i];
        if (e.size() > 2 && e.front() == '<' && e.back() == '>') {
            const std::string num = e.substr(1, e.size() - 2);
            char* end = nullptr;
            const float f = std::strtof(num.c_str(), &end);
            if (end && *end == '\0') ids.push_back(floatToBits(f));
            continue;
        }
        if (e.find_first_of("*?") != std::string::npos) {
            for (size_t m = 0; m < manifest.size(); ++m) {
                if (wildcardMatch(e.c_str(), manifest[m].first.c_str())) ids.push_back(manifest[m].second);
            }
            continue;
        }
        bool found = false;
        for (size_t m = 0; m < manifest.size(); ++m) {
            if (manifest[m].first == e) { ids.push_back(manifest[m].second); found = true; break; }
        }
        if (!found) ids.push_back(nameToIdBits(e));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

inline bool containsId(const std::vector<uint32_t>& sortedIds, uint32_t bits)
{
    return std::binary_search(sortedIds.begin(), sortedIds.end(), bits);
}

// Layer family from a plane name: "CryptoObject00" -> ("CryptoObject", 0).
// Accepts an optional '.'/'_' before the rank digits ("crypto_object00").
inline bool splitRankedLayer(const std::string& plane, std::string* family, int* rank)
{
    if (plane.size() < 3) return false;
    const size_t n = plane.size();
    if (!std::isdigit((unsigned char)plane[n - 1]) || !std::isdigit((unsigned char)plane[n - 2])) return false;
    if (n >= 3 && std::isdigit((unsigned char)plane[n - 3])) return false;   // exactly two digits
    *rank = (plane[n - 2] - '0') * 10 + (plane[n - 1] - '0');
    *family = plane.substr(0, n - 2);
    return !family->empty();
}

} // namespace CryptomatteCore

#endif // NATRON_ENGINE_CRYPTOMATTECORE_H
