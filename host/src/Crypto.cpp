// ============================================================================
// Crypto.cpp - BigInt / RSA PKCS#1 v1.5 实现（零第三方依赖）
// ============================================================================

#include "sai2/host/Crypto.h"
#include "sai2/host/PluginSecurity.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>

namespace sai2::host {

namespace {

constexpr uint64_t kLimbBase = 1ull << 32;

uint32_t Lo32(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFu); }
uint32_t Hi32(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

// 两个 (2^32) 进制数的十进制/十六进制辅助
std::string HexNibblesToBytes(const std::string& hex, std::vector<uint8_t>& bytes) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    bytes.clear();
    // 从右往左每两个 hex 一个字节
    int i = static_cast<int>(hex.size()) - 1;
    while (i >= 0) {
        const int hi = (i - 1 >= 0) ? val(hex[i - 1]) : 0;
        const int lo = val(hex[i]);
        if (lo < 0 || hi < 0) return "非法 hex 字符";
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        i -= 2;
    }
    return {};
}

// SHA-256 DigestInfo 前缀（RFC 8017，PKCS#1 v1.5）
constexpr uint8_t kSha256DigestInfo[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

} // namespace

// ---------------------------------------------------------------------------
// BigInt
// ---------------------------------------------------------------------------

BigInt::BigInt(uint64_t v) {
    if (v) {
        limb.push_back(Lo32(v));
        if (Hi32(v)) limb.push_back(Hi32(v));
    }
}

bool BigInt::IsZero() const {
    return limb.empty() || (limb.size() == 1 && limb[0] == 0);
}

void BigInt::Trim() {
    while (!limb.empty() && limb.back() == 0) limb.pop_back();
}

size_t BigInt::BitLength() const {
    size_t i = limb.size();
    while (i > 0 && limb[i - 1] == 0) --i;
    if (i == 0) return 0;
    uint32_t top = limb[i - 1];
    size_t bits = (i - 1) * 32;
    while (top) { ++bits; top >>= 1; }
    return bits;
}

int BigInt::Compare(const BigInt& o) const {
    size_t na = limb.size(), nb = o.limb.size();
    while (na > 0 && limb[na - 1] == 0) --na;
    while (nb > 0 && o.limb[nb - 1] == 0) --nb;
    if (na != nb) return na < nb ? -1 : 1;
    for (size_t i = na; i-- > 0;) {
        if (limb[i] != o.limb[i]) return limb[i] < o.limb[i] ? -1 : 1;
    }
    return 0;
}

BigInt BigInt::Add(const BigInt& a, const BigInt& b) {
    BigInt r;
    r.limb.assign(std::max(a.limb.size(), b.limb.size()) + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < r.limb.size(); ++i) {
        uint64_t s = carry;
        if (i < a.limb.size()) s += a.limb[i];
        if (i < b.limb.size()) s += b.limb[i];
        r.limb[i] = Lo32(s);
        carry = Hi32(s);
    }
    r.Trim();
    return r;
}

BigInt BigInt::Sub(const BigInt& a, const BigInt& b) {
    BigInt r;
    r.limb.assign(a.limb.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < a.limb.size(); ++i) {
        int64_t d = static_cast<int64_t>(a.limb[i]) - borrow;
        if (i < b.limb.size()) d -= static_cast<int64_t>(b.limb[i]);
        if (d < 0) { d += kLimbBase; borrow = 1; } else borrow = 0;
        r.limb[i] = static_cast<uint32_t>(d);
    }
    r.Trim();
    return r;
}

BigInt BigInt::Mul(const BigInt& a, const BigInt& b) {
    if (a.IsZero() || b.IsZero()) return BigInt();
    BigInt r;
    r.limb.assign(a.limb.size() + b.limb.size(), 0);
    for (size_t i = 0; i < a.limb.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.limb.size(); ++j) {
            const uint64_t cur =
                static_cast<uint64_t>(r.limb[i + j]) +
                static_cast<uint64_t>(a.limb[i]) * b.limb[j] + carry;
            r.limb[i + j] = Lo32(cur);
            carry = Hi32(cur);
        }
        r.limb[i + b.limb.size()] += Lo32(carry);
        // carry 高位不会溢出更多 limb（i 循环内累计）
    }
    r.Trim();
    return r;
}

void BigInt::DivMod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r) {
    q = BigInt();
    r = BigInt();
    if (b.IsZero()) return; // 调用方避免除零
    if (a.Compare(b) < 0) { r = a; r.Trim(); return; }

    // 逐位长除法（从高位到低位）
    const size_t bits = a.BitLength();
    for (size_t i = bits; i-- > 0;) {
        // r = r << 1（低位补 0，进位从低位传向高位）
        uint32_t carry = 0;
        for (size_t k = 0; k < r.limb.size(); ++k) {
            const uint32_t next = r.limb[k] >> 31;
            r.limb[k] = (r.limb[k] << 1) | carry;
            carry = next;
        }
        if (carry) r.limb.push_back(1);
        // r |= bit(a, i)
        if ((a.limb[i >> 5] >> (i & 31)) & 1u) {
            if (r.limb.empty()) r.limb.push_back(1);
            else r.limb[0] |= 1u;
        }
        r.Trim();
        if (r.Compare(b) >= 0) {
            r = BigInt::Sub(r, b);
            if (q.limb.empty()) q.limb.assign((i >> 5) + 1, 0);
            q.limb[i >> 5] |= (1u << (i & 31));
        }
    }
    q.Trim();
    r.Trim();
}

BigInt BigInt::Mod(const BigInt& a, const BigInt& b) {
    BigInt q, r;
    DivMod(a, b, q, r);
    return r;
}

BigInt BigInt::ModPow(const BigInt& base, const BigInt& exp,
                      const BigInt& mod) {
    BigInt result(1);
    BigInt b = Mod(base, mod);
    const size_t bits = exp.BitLength();
    for (size_t i = 0; i < bits; ++i) {
        if ((exp.limb[i >> 5] >> (i & 31)) & 1u)
            result = Mod(Mul(result, b), mod);
        b = Mod(Mul(b, b), mod);
    }
    return result;
}

BigInt BigInt::FromHex(const std::string& hex) {
    BigInt r;
    std::vector<uint8_t> bytes;
    if (!HexNibblesToBytes(hex, bytes).empty()) return r;
    r.limb.assign((bytes.size() + 3) / 4, 0);
    for (size_t k = 0; k < bytes.size(); ++k)
        r.limb[k >> 2] |= static_cast<uint32_t>(bytes[k]) << ((k & 3) * 8);
    r.Trim();
    return r;
}

std::string BigInt::ToHex() const {
    if (IsZero()) return "0";
    size_t n = limb.size();
    while (n > 0 && limb[n - 1] == 0) --n;
    std::string out;
    out.reserve(n * 8);
    static const char* d = "0123456789abcdef";
    bool first = true;
    for (size_t i = n; i-- > 0;) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            const uint8_t nib = (limb[i] >> shift) & 0xF;
            if (first) {
                if (nib == 0) continue;
                first = false;
            }
            out.push_back(d[nib]);
        }
    }
    return out;
}

bool BigInt::IsProbablyPrime(const BigInt& n, int rounds) {
    if (n.Compare(BigInt(2)) < 0) return false;   // 0、1
    if (n.Compare(BigInt(3)) <= 0) return true;   // 2、3
    if (!(n.limb[0] & 1u)) return false;          // 偶数

    // 小素数快速筛
    for (uint32_t sp : {3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 37u}) {
        if (BigInt::Mod(n, BigInt(sp)).IsZero())
            return n.Compare(BigInt(sp)) == 0;
    }

    // n-1 = 2^s * d
    BigInt nm1 = BigInt::Sub(n, BigInt(1));
    BigInt d = nm1;
    size_t s = 0;
    while (!d.IsZero() && (d.limb[0] & 1u) == 0) {
        BigInt q, r;
        BigInt::DivMod(d, BigInt(2), q, r);
        d = q;
        ++s;
    }

    std::mt19937_64 rng(static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) ^
        0x9E3779B97F4A7C15ull);

    for (int round = 0; round < rounds; ++round) {
        // 随机 a ∈ [2, n-2]（a 在每次尝试时重建，规避 Mod 结果为零时空 limb）
        bool ok = false;
        BigInt a;
        for (int attempt = 0; attempt < 64 && !ok; ++attempt) {
            a = BigInt();
            a.limb.assign(n.limb.size(), 0);
            for (size_t k = 0; k < a.limb.size(); ++k)
                a.limb[k] = static_cast<uint32_t>(rng());
            a.limb[0] |= 2u; // ≥ 2
            a.Trim();
            a = BigInt::Mod(a, n);
            if (a.Compare(BigInt(1)) > 0 && a.Compare(nm1) < 0) ok = true;
        }
        if (!ok) return false;

        BigInt x = BigInt::ModPow(a, d, n);
        if (x.Compare(BigInt(1)) == 0 || x.Compare(nm1) == 0) continue;
        bool composite = true;
        for (size_t r = 1; r < s; ++r) {
            x = BigInt::Mod(BigInt::Mul(x, x), n);
            if (x.Compare(nm1) == 0) { composite = false; break; }
            if (x.Compare(BigInt(1)) == 0) return false;
        }
        if (composite) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// RSA 密钥生成
// ---------------------------------------------------------------------------

namespace {

BigInt GenPrime(int bits, std::mt19937_64& rng) {
    for (int attempt = 0; attempt < 100000; ++attempt) {
        BigInt c;
        c.limb.assign((bits + 31) / 32, 0);
        for (size_t k = 0; k < c.limb.size(); ++k)
            c.limb[k] = static_cast<uint32_t>(rng());
        // 置位最高位与次高位（保证位数、避免接近 2^(bits-1) 的低熵因子）
        const size_t topLimb = c.limb.size() - 1;
        const int topShift = (bits - 1) & 31;
        c.limb[topLimb] |= (1u << topShift);
        if (topShift > 0) c.limb[topLimb] |= (1u << (topShift - 1));
        c.limb[0] |= 1u; // 奇数
        // bits 非 32 倍数时清掉多余高位
        const int extraHigh = (c.limb.size() * 32) - bits;
        if (extraHigh > 0) c.limb[topLimb] &= (0xFFFFFFFFu >> extraHigh);
        c.limb[topLimb] |= (1u << topShift);
        c.Trim();
        if (BigInt::IsProbablyPrime(c)) return c;
    }
    return BigInt();
}

// (a - b) mod m，要求 a、b 均已规约到 [0, m)
BigInt ModSub(const BigInt& a, const BigInt& b, const BigInt& m) {
    if (a.Compare(b) >= 0) return BigInt::Sub(a, b);
    const BigInt diff = BigInt::Sub(b, a);
    if (diff.IsZero()) return BigInt();
    return BigInt::Sub(m, diff);
}

BigInt ExtGcdModInverse(const BigInt& a, const BigInt& m) {
    // 扩展欧几里得（迭代）。不变量：s_i * a ≡ r_i (mod m)，系数全程规约到 [0,m)
    BigInt r0 = BigInt::Mod(a, m), r1 = m;
    BigInt s0(1), s1(0);
    while (!r1.IsZero()) {
        BigInt q, rem;
        BigInt::DivMod(r0, r1, q, rem);
        r0 = r1; r1 = rem;
        const BigInt sNext = ModSub(s0, BigInt::Mod(BigInt::Mul(q, s1), m), m);
        s0 = s1;
        s1 = sNext;
    }
    return BigInt::Mod(s0, m); // r0 == gcd == 1 时 s0 即逆元
}

} // namespace

bool RsaGenerateKeyPair(int bits, RsaKeyPair& out, std::string& error) {
    if (bits < 512) { error = "密钥位数至少 512"; return false; }
    const int half = bits / 2;
    std::mt19937_64 rng(static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) ^
        0xC0FFEE123456789Full);

    BigInt e(65537);
    for (int attempt = 0; attempt < 64; ++attempt) {
        BigInt p = GenPrime(half, rng);
        BigInt q = GenPrime(half, rng);
        if (p.IsZero() || q.IsZero() || p.Compare(q) == 0) continue;
        BigInt n = BigInt::Mul(p, q);
        if (static_cast<int>(n.BitLength()) != bits) continue;
        BigInt phi = BigInt::Mul(BigInt::Sub(p, BigInt(1)),
                                 BigInt::Sub(q, BigInt(1)));
        if (BigInt::Mod(phi, e).IsZero()) continue; // gcd(phi,e) != 1
        BigInt d = ExtGcdModInverse(e, phi);
        if (d.IsZero()) continue;
        out = RsaKeyPair{n, e, d, p, q};
        return true;
    }
    error = "密钥生成失败（试了 64 次）";
    return false;
}

// ---------------------------------------------------------------------------
// PluginSignature
// ---------------------------------------------------------------------------

std::string PluginSignature::EntryMessage(const std::string& pluginId,
                                          const std::string& version,
                                          const std::string& sha256Hex) {
    return "SAI2-SIGN1|" + pluginId + "|" + version + "|" + sha256Hex;
}

bool PluginSignature::Sign(const BigInt& n, const BigInt& d,
                           const std::string& message,
                           std::string& sigHexOut, std::string& error) {
    const size_t k = n.limb.size() * 4; // EM 长度（字节）= 模数字节长
    if (k < 11 + sizeof(kSha256DigestInfo) + 32) {
        error = "模数太短，无法构造 PKCS#1 v1.5 编码";
        return false;
    }
    // EMSA-PKCS1-v1_5: 0x00 01 FF..FF 00 || DigestInfo || H
    std::vector<uint8_t> em(k, 0xFF);
    em[0] = 0x00; em[1] = 0x01;
    const size_t tLen = sizeof(kSha256DigestInfo) + 32;
    em[2 + (k - 3 - tLen)] = 0x00; // 0x00 分隔符位置
    size_t pos = 3 + (k - 3 - tLen);
    for (uint8_t b : kSha256DigestInfo) em[pos++] = b;
    const std::string hash = PluginSecurity::Sha256Bytes(
        reinterpret_cast<const uint8_t*>(message.data()), message.size());
    for (size_t i = 0; i < 32; ++i) {
        em[pos + i] = static_cast<uint8_t>(
            std::stoul(hash.substr(i * 2, 2), nullptr, 16));
    }

    BigInt m = BigInt::FromHex([&] {
        std::string s;
        for (uint8_t b : em) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            s += buf;
        }
        return s;
    }());
    const BigInt sig = BigInt::ModPow(m, d, n);
    sigHexOut = sig.ToHex();
    return true;
}

bool PluginSignature::Verify(const std::string& pubKeyText,
                             const std::string& sigHex,
                             const std::string& message,
                             std::string& error) {
    error.clear();
    BigInt n, e;
    if (!DecodePublicKey(pubKeyText, n, e, error)) return false;

    // 签名 hex -> BigInt -> EM
    const BigInt sig = BigInt::FromHex(sigHex);
    if (sig.Compare(n) >= 0) { error = "签名数值超出模数范围"; return false; }
    const BigInt emBig = BigInt::ModPow(sig, e, n);

    // 还原 EM 字节序列（模数字节长度，大端）
    const size_t k = n.limb.size() * 4;
    std::vector<uint8_t> em(k, 0);
    const std::string hex = emBig.ToHex();
    // hex 是去掉前导零的大端书写
    const size_t sigBytes = (hex.size() + 1) / 2;
    size_t off = k - sigBytes;
    auto nibVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int i = static_cast<int>(hex.size()) - 1;
    while (i >= 0) {
        const int hi = (i - 1 >= 0) ? nibVal(hex[i - 1]) : 0;
        const int lo = nibVal(hex[i]);
        if (lo < 0) { error = "签名 hex 非法"; return false; }
        em[off + static_cast<size_t>(i) / 2] =
            static_cast<uint8_t>(((i - 1 >= 0 ? hi : 0) << 4) | lo);
        i -= 2;
    }

    // 校验 PKCS#1 v1.5 结构
    if (k < 11) { error = "EM 太短"; return false; }
    if (em[0] != 0x00 || em[1] != 0x01) { error = "签名块头不匹配"; return false; }
    const size_t tLen = sizeof(kSha256DigestInfo) + 32;
    if (k < 3 + 8 + tLen) { error = "EM 长度不足"; return false; }
    // PS 必须全 0xFF
    size_t psEnd = 2;
    while (psEnd < k && em[psEnd] == 0xFF) ++psEnd;
    if (psEnd < 10 || psEnd >= k || em[psEnd] != 0x00) {
        error = "PS 填充不合法"; return false;
    }
    if (k - (psEnd + 1) != tLen) { error = "DigestInfo 长度不匹配"; return false; }
    for (size_t t = 0; t < sizeof(kSha256DigestInfo); ++t) {
        if (em[psEnd + 1 + t] != kSha256DigestInfo[t]) {
            error = "DigestInfo 不匹配"; return false;
        }
    }
    // 比对哈希
    const std::string expect = PluginSecurity::Sha256Bytes(
        reinterpret_cast<const uint8_t*>(message.data()), message.size());
    std::string got;
    for (size_t t = 0; t < 32; ++t) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", em[psEnd + 1 + sizeof(kSha256DigestInfo) + t]);
        got += buf;
    }
    if (got != expect) { error = "消息哈希不匹配"; return false; }
    return true;
}

std::string PluginSignature::EncodePublicKey(const BigInt& n, const BigInt& e) {
    return "SAI2RSA1." + n.ToHex() + "." + e.ToHex();
}

bool PluginSignature::DecodePublicKey(const std::string& text, BigInt& n,
                                      BigInt& e, std::string& error) {
    const std::string prefix = "SAI2RSA1.";
    if (text.rfind(prefix, 0) != 0) { error = "公钥格式错误"; return false; }
    const size_t dot1 = prefix.size();
    const size_t dot2 = text.find('.', dot1);
    if (dot2 == std::string::npos) { error = "公钥格式错误"; return false; }
    n = BigInt::FromHex(text.substr(dot1, dot2 - dot1));
    e = BigInt::FromHex(text.substr(dot2 + 1));
    if (n.IsZero() || e.IsZero()) { error = "公钥数值为空"; return false; }
    return true;
}

} // namespace sai2::host
