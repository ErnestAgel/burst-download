/**
 * @file sha256.cpp
 * @brief SHA-256 implementation (FIPS 180-4), dependency-free.
 *
 * @author ErnestAgel
 * @date 2026-08-12
 * @license SPDX-License-Identifier: MIT
 */

#include "sha256.h"

#include <cstdio>

/** @brief Rotate a u32 right by n bits. */
static u32 RotateRight(u32 dwValue, u32 nBits)
{
    return (dwValue >> nBits) | (dwValue << (32 - nBits));
}

/** @brief SHA-256 round constants (first 32 bits of the cube roots of the
 *         first 64 primes). */
static const u32 kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/** @brief Load a big-endian u32 from bytes. */
static u32 LoadBe32(const u8* pbyData)
{
    return ((u32)pbyData[0] << 24) | ((u32)pbyData[1] << 16) |
           ((u32)pbyData[2] << 8) | (u32)pbyData[3];
}

/** @brief Store a u32 big-endian into bytes. */
static void StoreBe32(u8* pbyOut, u32 dwValue)
{
    pbyOut[0] = (u8)(dwValue >> 24);
    pbyOut[1] = (u8)(dwValue >> 16);
    pbyOut[2] = (u8)(dwValue >> 8);
    pbyOut[3] = (u8)dwValue;
}

/** @brief Compress one 64-byte block. */
static void Sha256Compress(TSha256Ctx& tCtx, const u8* pbyBlock)
{
    u32 dwW[64];
    for (u32 nIndex = 0; nIndex < 16; ++nIndex)
    {
        dwW[nIndex] = LoadBe32(pbyBlock + nIndex * 4);
    }
    for (u32 nIndex = 16; nIndex < 64; ++nIndex)
    {
        const u32 s0 = RotateRight(dwW[nIndex - 15], 7) ^
                       RotateRight(dwW[nIndex - 15], 18) ^
                       (dwW[nIndex - 15] >> 3);
        const u32 s1 = RotateRight(dwW[nIndex - 2], 17) ^
                       RotateRight(dwW[nIndex - 2], 19) ^
                       (dwW[nIndex - 2] >> 10);
        dwW[nIndex] = dwW[nIndex - 16] + s0 + dwW[nIndex - 7] + s1;
    }

    u32 dwA = tCtx.dwState[0];
    u32 dwB = tCtx.dwState[1];
    u32 dwC = tCtx.dwState[2];
    u32 dwD = tCtx.dwState[3];
    u32 dwE = tCtx.dwState[4];
    u32 dwF = tCtx.dwState[5];
    u32 dwG = tCtx.dwState[6];
    u32 dwH = tCtx.dwState[7];

    for (u32 nIndex = 0; nIndex < 64; ++nIndex)
    {
        const u32 s1 = RotateRight(dwE, 6) ^ RotateRight(dwE, 11) ^
                       RotateRight(dwE, 25);
        const u32 ch = (dwE & dwF) ^ ((~dwE) & dwG);
        const u32 t1 = dwH + s1 + ch + kRoundConstants[nIndex] + dwW[nIndex];
        const u32 s0 = RotateRight(dwA, 2) ^ RotateRight(dwA, 13) ^
                       RotateRight(dwA, 22);
        const u32 maj = (dwA & dwB) ^ (dwA & dwC) ^ (dwB & dwC);
        const u32 t2 = s0 + maj;
        dwH = dwG;
        dwG = dwF;
        dwF = dwE;
        dwE = dwD + t1;
        dwD = dwC;
        dwC = dwB;
        dwB = dwA;
        dwA = t1 + t2;
    }

    tCtx.dwState[0] += dwA;
    tCtx.dwState[1] += dwB;
    tCtx.dwState[2] += dwC;
    tCtx.dwState[3] += dwD;
    tCtx.dwState[4] += dwE;
    tCtx.dwState[5] += dwF;
    tCtx.dwState[6] += dwG;
    tCtx.dwState[7] += dwH;
}

void Sha256Init(TSha256Ctx& tCtx)
{
    tCtx.dwState[0] = 0x6a09e667;
    tCtx.dwState[1] = 0xbb67ae85;
    tCtx.dwState[2] = 0x3c6ef372;
    tCtx.dwState[3] = 0xa54ff53a;
    tCtx.dwState[4] = 0x510e527f;
    tCtx.dwState[5] = 0x9b05688c;
    tCtx.dwState[6] = 0x1f83d9ab;
    tCtx.dwState[7] = 0x5be0cd19;
    tCtx.dwBufLen = 0;
    tCtx.qwTotalLen = 0;
}

void Sha256Update(TSha256Ctx& tCtx, const u8* pbyData, size_t nLen)
{
    tCtx.qwTotalLen += nLen;
    size_t nIndex = 0;
    if (tCtx.dwBufLen > 0)
    {
        const size_t nNeed = 64 - tCtx.dwBufLen;
        const size_t nCopy = (nLen < nNeed) ? nLen : nNeed;
        for (size_t n = 0; n < nCopy; ++n)
        {
            tCtx.byBuf[tCtx.dwBufLen + n] = pbyData[n];
        }
        tCtx.dwBufLen += (u32)nCopy;
        nIndex += nCopy;
        if (tCtx.dwBufLen == 64)
        {
            Sha256Compress(tCtx, tCtx.byBuf);
            tCtx.dwBufLen = 0;
        }
    }
    while (nIndex + 64 <= nLen)
    {
        Sha256Compress(tCtx, pbyData + nIndex);
        nIndex += 64;
    }
    while (nIndex < nLen)
    {
        tCtx.byBuf[tCtx.dwBufLen++] = pbyData[nIndex++];
    }
}

void Sha256Final(TSha256Ctx& tCtx, u8 pbyDigest[32])
{
    const u64 qwBitLen = tCtx.qwTotalLen * 8;
    u8 byPad[72];
    byPad[0] = 0x80;
    const size_t nPad = (tCtx.dwBufLen < 56) ? (56 - tCtx.dwBufLen)
                                              : (120 - tCtx.dwBufLen);
    for (size_t n = 1; n < nPad; ++n)
    {
        byPad[n] = 0;
    }
    for (s32 n = 0; n < 8; ++n)
    {
        byPad[nPad + n] = (u8)(qwBitLen >> (56 - n * 8));
    }
    Sha256Update(tCtx, byPad, nPad + 8);
    for (u32 n = 0; n < 8; ++n)
    {
        StoreBe32(pbyDigest + n * 4, tCtx.dwState[n]);
    }
}

std::string Sha256Hex(const u8 pbyDigest[32])
{
    static const char kHex[] = "0123456789abcdef";
    std::string strOut;
    strOut.reserve(64);
    for (u32 n = 0; n < 32; ++n)
    {
        strOut += kHex[pbyDigest[n] >> 4];
        strOut += kHex[pbyDigest[n] & 0x0F];
    }
    return strOut;
}
