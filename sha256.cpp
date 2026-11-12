// Isiliel — scripting language interpreter/compiler.
// Embedded: sandbox, embedding API, OOP, 2D graphics and REPL.
// Authors: EEPS and MiMo-V2.6 | Version: 1.0 | Date: 2026-11-08

// SHA-256 (FIPS 180-4) — own implementation (public domain, compact
// strefork/schneier style). Used to verify `integrity` in .mod modules.

#include "sha256.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

using u8 = unsigned char;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

struct Sha256 {
    u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8  buf[64];
    u64 len = 0;          // total bytes
    size_t buflen = 0;    // bytes in buf

    void block(const u8* p) {
        u32 w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (u32(p[i * 4]) << 24) | (u32(p[i * 4 + 1]) << 16) |
                   (u32(p[i * 4 + 2]) << 8) | u32(p[i * 4 + 3]);
        for (int i = 16; i < 64; i++) {
            u32 s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            u32 s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        u32 e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            u32 ch = (e & f) ^ (~e & g);
            u32 t1 = hh + S1 + ch + K[i] + w[i];
            u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            u32 maj = (a & b) ^ (a & c) ^ (b & c);
            u32 t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const u8* p, size_t n) {
        len += n;
        while (n > 0) {
            size_t take = 64 - buflen;
            if (take > n) take = n;
            for (size_t i = 0; i < take; i++) buf[buflen + i] = p[i];
            buflen += take; p += take; n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    std::string finish() {
        u64 bits = len * 8;
        u8 pad = 0x80;
        update(&pad, 1);
        u8 zero = 0;
        while (buflen != 56) update(&zero, 1);
        u8 lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = u8(bits >> (56 - 8 * i));
        update(lenb, 8);
        char out[65];
        for (int i = 0; i < 8; i++)
            std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

} // namespace

std::string sha256_hex(const std::string& data) {
    Sha256 s;
    s.update(reinterpret_cast<const u8*>(data.data()), data.size());
    return s.finish();
}

std::string sha256_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("sha256: nao foi possivel abrir '" + path + "'");
    Sha256 s;
    std::vector<char> buf(8192);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize got = f.gcount();
        if (got > 0)
            s.update(reinterpret_cast<const u8*>(buf.data()), (size_t)got);
    }
    return s.finish();
}
