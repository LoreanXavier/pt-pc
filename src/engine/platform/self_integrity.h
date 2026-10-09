#pragma once
// The setup checks its own file against a corrupt or truncated download: tools/ci/stamp_integrity.py writes the SHA-256
// of the file (with this digest's 32 bytes zeroed) after the marker below once the build is done; at run time the file is
// read back, the digest taken out, the same hash recomputed and compared. A damaged setup (or one never stamped) refuses
// to install.
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pt::integrity {
// 16 marker bytes, then the 32 byte digest (zero until stamped)
inline volatile unsigned char kIntegrity[48] = {0x7E, 0x50, 0x54, 0x2D, 0x49, 0x4E, 0x54, 0x45, 0x47, 0x52, 0x49, 0x54, 0x59, 0x2D, 0x76, 0x31};

#ifdef _WIN32
inline bool Sha256(const unsigned char* data, size_t size, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0 && BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
#else
// SHA-256 (FIPS 180-4) for the platforms without BCrypt
inline bool Sha256(const unsigned char* data, size_t size, unsigned char out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const unsigned char* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    size_t done = 0;
    for (; size - done >= 64; done += 64) block(data + done);
    unsigned char tail[128] = {};
    const size_t rest = size - done;
    std::memcpy(tail, data + done, rest);
    tail[rest] = 0x80;
    const size_t tail_size = rest < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    block(tail);
    if (tail_size == 128) block(tail + 64);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<unsigned char>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<unsigned char>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<unsigned char>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<unsigned char>(h[i]);
    }
    return true;
}
#endif

inline bool IntegrityOk() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
#else
    const char* path = "/proc/self/exe";
#endif
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    unsigned char marker[16];
    for (int i = 0; i < 16; ++i) marker[i] = kIntegrity[i];
    size_t at = std::string::npos;
    for (size_t i = 0; i + 48 <= bytes.size(); ++i) {
        if (std::memcmp(&bytes[i], marker, 16) == 0) {
            if (at != std::string::npos) return false;
            at = i;
        }
    }
    if (at == std::string::npos) return false;
    unsigned char stored[32];
    std::memcpy(stored, &bytes[at + 16], 32);
    bool stamped = false;
    for (unsigned char b : stored) stamped |= b != 0;
    if (!stamped) return false;
    std::memset(&bytes[at + 16], 0, 32);
    unsigned char digest[32];
    if (!Sha256(bytes.data(), bytes.size(), digest)) return false;
    unsigned char diff = 0;
    for (int i = 0; i < 32; ++i) diff |= digest[i] ^ stored[i];
    return diff == 0;
}
}
