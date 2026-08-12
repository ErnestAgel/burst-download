#ifndef BURST_SHA256_H
#define BURST_SHA256_H

/**
 * @file sha256.h
 * @brief Minimal self-contained SHA-256 implementation (FIPS 180-4).
 *
 * Kept dependency-free so the download engine can verify file integrity
 * (issue O4) on every platform without relying on the trimmed FFmpeg build.
 */

#include <cstddef>
#include <string>

#include "burst_types.h"

/** @brief SHA-256 context (opaque state for the streaming API). */
typedef struct tagSha256Ctx
{
    u32 dwState[8];   /**< Working state */
    u32 dwBufLen;     /**< Bytes currently buffered */
    u8  byBuf[64];    /**< Pending input buffer */
    u64 qwTotalLen;   /**< Total bytes processed */
} TSha256Ctx;

/**
 * @brief Initialize a SHA-256 context.
 * @param tCtx Context to initialize.
 */
void Sha256Init(TSha256Ctx& tCtx);

/**
 * @brief Feed bytes into the hash.
 * @param tCtx Context.
 * @param pbyData Input bytes.
 * @param nLen Input length.
 */
void Sha256Update(TSha256Ctx& tCtx, const u8* pbyData, size_t nLen);

/**
 * @brief Finalize and write the 32-byte digest.
 * @param tCtx Context.
 * @param pbyDigest Output digest (exactly 32 bytes).
 */
void Sha256Final(TSha256Ctx& tCtx, u8 pbyDigest[32]);

/**
 * @brief Render a digest as lowercase hex.
 * @param pbyDigest 32-byte digest.
 * @return 64-char hex string.
 */
std::string Sha256Hex(const u8 pbyDigest[32]);

#endif  // BURST_SHA256_H
