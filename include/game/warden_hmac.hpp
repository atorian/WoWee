#pragma once

/// HMAC-SHA1 keyed once and applied to many short messages.
///
/// Warden's code-pattern checks (PAGE_A/PAGE_B) send a 4-byte seed and the
/// HMAC of a few dozen bytes somewhere in WoW.exe; the only way to answer is
/// to HMAC every position of the image and compare. OpenSSL's one-shot HMAC()
/// re-derives the padded key at each of those ten million positions. This
/// absorbs the key's ipad and opad blocks once and resumes from copies of
/// those states, still letting OpenSSL do the compressions (with the CPU's
/// SHA instructions where it has them): about twelve times faster per thread.
///
/// Not thread-safe: give each thread its own instance - constructing one is
/// two compressions.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <openssl/evp.h>

namespace wowee::game {

class WardenHmacSha1 {
public:
    /// keyLen must be at most 64 bytes (Warden's seed is 4).
    WardenHmacSha1(const uint8_t* key, size_t keyLen)
        : inner_(EVP_MD_CTX_new()), outer_(EVP_MD_CTX_new()), work_(EVP_MD_CTX_new()) {
        uint8_t ipad[64], opad[64];
        std::memset(ipad, 0x36, sizeof(ipad));
        std::memset(opad, 0x5c, sizeof(opad));
        for (size_t i = 0; i < keyLen && i < 64; ++i) {
            ipad[i] ^= key[i];
            opad[i] ^= key[i];
        }
        ok_ = inner_ && outer_ && work_ &&
              EVP_DigestInit_ex(inner_, EVP_sha1(), nullptr) && EVP_DigestUpdate(inner_, ipad, 64) &&
              EVP_DigestInit_ex(outer_, EVP_sha1(), nullptr) && EVP_DigestUpdate(outer_, opad, 64);
    }
    ~WardenHmacSha1() {
        EVP_MD_CTX_free(inner_);
        EVP_MD_CTX_free(outer_);
        EVP_MD_CTX_free(work_);
    }
    WardenHmacSha1(const WardenHmacSha1&) = delete;
    WardenHmacSha1& operator=(const WardenHmacSha1&) = delete;

    /// HMAC of msg into out. False only if OpenSSL failed, which leaves out
    /// unspecified - callers treat that as no match.
    bool digest(const uint8_t* msg, size_t len, uint8_t out[20]) {
        uint8_t inner[20];
        unsigned size = 0;
        return ok_ &&
               EVP_MD_CTX_copy_ex(work_, inner_) && EVP_DigestUpdate(work_, msg, len) &&
               EVP_DigestFinal_ex(work_, inner, &size) && size == 20 &&
               EVP_MD_CTX_copy_ex(work_, outer_) && EVP_DigestUpdate(work_, inner, 20) &&
               EVP_DigestFinal_ex(work_, out, &size) && size == 20;
    }

private:
    EVP_MD_CTX* inner_;
    EVP_MD_CTX* outer_;
    EVP_MD_CTX* work_;
    bool ok_ = false;
};

} // namespace wowee::game
