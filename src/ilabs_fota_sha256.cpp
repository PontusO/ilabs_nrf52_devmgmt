// iLabs LTE FOTA -- streaming SHA-256 (implementation).
//
// Independent implementation of NIST FIPS 180-4 SHA-256. Public-domain
// algorithmic reference; this code is original. No mbedTLS source was
// copied -- only the API surface matches so the call sites are
// drop-in compatible with upstream mbedTLS_sha256_*.
//
// Library extraction note: this file moves verbatim to
// `src/ilabs_fota_sha256.cpp`. Standard C only; portable to any
// C99/C++11 target.

#include "ilabs_fota_sha256.h"

#include <string.h>

// -------- FIPS 180-4 constants --------

// SHA-256 initial hash values (FIPS 180-4 §5.3.3): the first 32 bits of
// the fractional parts of the square roots of the first 8 primes.
static const uint32_t kInitialH[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

// SHA-256 round constants (FIPS 180-4 §4.2.2): the first 32 bits of the
// fractional parts of the cube roots of the first 64 primes.
static const uint32_t kK[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

// -------- inline ops --------

static inline uint32_t ror32(uint32_t v, unsigned n) {
    return (v >> n) | (v << (32 - n));
}

// FIPS 180-4 §4.1.2
static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
static inline uint32_t BigSigma0(uint32_t x) {
    return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22);
}
static inline uint32_t BigSigma1(uint32_t x) {
    return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25);
}
static inline uint32_t SmallSigma0(uint32_t x) {
    return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3);
}
static inline uint32_t SmallSigma1(uint32_t x) {
    return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10);
}

// Process one 64-byte block. block must be exactly 64 bytes.
static void process_block(ilabs_sha256_ctx* ctx, const uint8_t* block) {
    uint32_t w[64];

    // Schedule: first 16 words are the block in big-endian.
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4    ] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] <<  8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    // Words 16..63 derived.
    for (int i = 16; i < 64; ++i) {
        w[i] = SmallSigma1(w[i - 2]) + w[i - 7] +
               SmallSigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + BigSigma1(e) + Ch(e, f, g) + kK[i] + w[i];
        uint32_t t2 = BigSigma0(a) + Maj(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

// -------- public API --------

void ilabs_sha256_init(ilabs_sha256_ctx* ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

void ilabs_sha256_free(ilabs_sha256_ctx* ctx) {
    if (!ctx) return;
    // Zeroise to avoid leaving sensitive intermediate state in RAM.
    // (For SHA-256 the "secret" is the partial digest. Cheap insurance.)
    memset(ctx, 0, sizeof(*ctx));
}

void ilabs_sha256_starts(ilabs_sha256_ctx* ctx) {
    if (!ctx) return;
    ctx->total_len  = 0;
    ctx->buffer_len = 0;
    memcpy(ctx->state, kInitialH, sizeof(kInitialH));
}

void ilabs_sha256_update(ilabs_sha256_ctx* ctx,
                           const uint8_t* data, size_t len) {
    if (!ctx || len == 0 || data == nullptr) return;

    ctx->total_len += len;

    // Top up partial buffer if there's any held over.
    if (ctx->buffer_len > 0) {
        size_t fill = ILABS_SHA256_BLOCK_SIZE - ctx->buffer_len;
        if (fill > len) fill = len;
        memcpy(ctx->buffer + ctx->buffer_len, data, fill);
        ctx->buffer_len += fill;
        data += fill;
        len  -= fill;
        if (ctx->buffer_len == ILABS_SHA256_BLOCK_SIZE) {
            process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }

    // Process full blocks from the input directly.
    while (len >= ILABS_SHA256_BLOCK_SIZE) {
        process_block(ctx, data);
        data += ILABS_SHA256_BLOCK_SIZE;
        len  -= ILABS_SHA256_BLOCK_SIZE;
    }

    // Stash any tail.
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

void ilabs_sha256_finish(ilabs_sha256_ctx* ctx,
                           uint8_t digest[ILABS_SHA256_DIGEST_SIZE]) {
    if (!ctx || !digest) return;

    // Capture total bit-length before we mutate state.
    uint64_t total_bits = ctx->total_len * 8u;

    // Padding: append 0x80, then zeros, then 8-byte big-endian bit length,
    // so the final block ends at a 64-byte boundary.
    ctx->buffer[ctx->buffer_len++] = 0x80;

    // If there's not enough room for the 8-byte length in the current
    // block, pad to end of block, process it, then start a fresh block.
    if (ctx->buffer_len > ILABS_SHA256_BLOCK_SIZE - 8) {
        memset(ctx->buffer + ctx->buffer_len, 0,
               ILABS_SHA256_BLOCK_SIZE - ctx->buffer_len);
        process_block(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
    memset(ctx->buffer + ctx->buffer_len, 0,
           ILABS_SHA256_BLOCK_SIZE - 8 - ctx->buffer_len);

    // Append bit length, big-endian.
    for (int i = 0; i < 8; ++i) {
        ctx->buffer[ILABS_SHA256_BLOCK_SIZE - 1 - i] =
            (uint8_t)(total_bits >> (i * 8));
    }
    process_block(ctx, ctx->buffer);

    // Emit digest big-endian.
    for (int i = 0; i < 8; ++i) {
        digest[i * 4    ] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void ilabs_sha256(const uint8_t* data, size_t len,
                    uint8_t digest[ILABS_SHA256_DIGEST_SIZE]) {
    ilabs_sha256_ctx ctx;
    ilabs_sha256_init(&ctx);
    ilabs_sha256_starts(&ctx);
    ilabs_sha256_update(&ctx, data, len);
    ilabs_sha256_finish(&ctx, digest);
    ilabs_sha256_free(&ctx);
}

void ilabs_sha256_hex(const uint8_t digest[ILABS_SHA256_DIGEST_SIZE],
                        char out[2 * ILABS_SHA256_DIGEST_SIZE + 1]) {
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < ILABS_SHA256_DIGEST_SIZE; ++i) {
        out[i * 2    ] = hex_chars[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex_chars[ digest[i]       & 0xF];
    }
    out[2 * ILABS_SHA256_DIGEST_SIZE] = '\0';
}
