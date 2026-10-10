#include "sai2/host/PluginSecurity.h"
#include "sai2/host/PathUtil.h"

#include <fstream>
#include <vector>

namespace sai2::host {

// ---------------------------------------------------------------------------
// SHA-256（FIPS 180-4，独立实现，无外部依赖）
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64] = {};
    size_t bufLen = 0;
    uint64_t total = 0;

    void Block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 =
                Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 =
                Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Update(const uint8_t* data, size_t len) {
        total += len;
        while (len > 0) {
            const size_t take = (64 - bufLen) < len ? (64 - bufLen) : len;
            for (size_t i = 0; i < take; ++i) buf[bufLen + i] = data[i];
            bufLen += take;
            data += take;
            len -= take;
            if (bufLen == 64) {
                Block(buf);
                bufLen = 0;
            }
        }
    }

    void Final(uint8_t out[32]) {
        const uint64_t bits = total * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (bufLen != 56) Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = uint8_t((bits >> (56 - i * 8)) & 0xFF);
        Update(lenBytes, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = uint8_t((h[i] >> 24) & 0xFF);
            out[i * 4 + 1] = uint8_t((h[i] >> 16) & 0xFF);
            out[i * 4 + 2] = uint8_t((h[i] >> 8) & 0xFF);
            out[i * 4 + 3] = uint8_t(h[i] & 0xFF);
        }
    }
};

std::string ToHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(kHex[data[i] >> 4]);
        s.push_back(kHex[data[i] & 0xF]);
    }
    return s;
}

} // namespace

std::string PluginSecurity::Sha256Bytes(const uint8_t* data, size_t len) {
    Sha256 sha;
    sha.Update(data, len);
    uint8_t digest[32];
    sha.Final(digest);
    return ToHex(digest, 32);
}

bool PluginSecurity::Sha256File(const std::string& path, std::string& hexOut,
                                std::string& error) {
    // path 是 UTF-8：转宽字符再开流（目录含中文时必需）
    std::ifstream f(PathFromUtf8(path), std::ios::binary);
    if (!f) {
        error = "无法打开文件: " + path;
        return false;
    }
    Sha256 sha;
    std::vector<char> buf(64 * 1024);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0)
            sha.Update(reinterpret_cast<const uint8_t*>(buf.data()),
                       static_cast<size_t>(got));
    }
    uint8_t digest[32];
    sha.Final(digest);
    hexOut = ToHex(digest, 32);
    return true;
}

bool PluginSecurity::VerifyFileSha256(const std::string& path,
                                      const std::string& expectedHex,
                                      std::string& error) {
    std::string actual;
    if (!Sha256File(path, actual, error)) return false;
    if (actual.size() != expectedHex.size()) {
        error = "SHA-256 长度不匹配";
        return false;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        const char a = actual[i] >= 'A' && actual[i] <= 'F'
                           ? char(actual[i] - 'A' + 'a')
                           : actual[i];
        const char b = expectedHex[i] >= 'A' && expectedHex[i] <= 'F'
                           ? char(expectedHex[i] - 'A' + 'a')
                           : expectedHex[i];
        if (a != b) {
            error = "SHA-256 校验失败: " + path;
            return false;
        }
    }
    return true;
}

} // namespace sai2::host
