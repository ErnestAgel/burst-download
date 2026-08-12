/**
 * @file test_sha256.cpp
 * @brief Unit tests for the self-contained SHA-256 implementation using
 *        the standard FIPS test vectors.
 */

#include <string>

#include "sha256.h"
#include "test_framework.h"

/** @brief Compute the hex digest of a byte string. */
static std::string DigestHex(const std::string& strIn)
{
    TSha256Ctx tCtx;
    u8 byDigest[32];
    Sha256Init(tCtx);
    Sha256Update(tCtx, (const u8*)strIn.data(), strIn.size());
    Sha256Final(tCtx, byDigest);
    return Sha256Hex(byDigest);
}

/** @brief Test: FIPS vectors (empty string, "abc", 1M 'a' via streaming). */
static void TestSha256Vectors(CTestReport& cReport)
{
    cReport.BeginCase("sha256: FIPS vectors");
    BURST_EXPECT_STR_EQ(
        cReport,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        DigestHex(""));
    BURST_EXPECT_STR_EQ(
        cReport,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        DigestHex("abc"));
    BURST_EXPECT_STR_EQ(
        cReport,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        DigestHex(std::string(1000000, 'a')));
}

/** @brief Test: streaming updates produce the same digest as one shot. */
static void TestSha256Streaming(CTestReport& cReport)
{
    cReport.BeginCase("sha256: streaming");
    const std::string strInput =
        "Burst Download multi-threaded chunked downloader test payload";
    const std::string strOneShot = DigestHex(strInput);

    TSha256Ctx tCtx;
    u8 byDigest[32];
    Sha256Init(tCtx);
    for (size_t n = 0; n < strInput.size(); n += 7)
    {
        Sha256Update(tCtx, (const u8*)strInput.data() + n,
                     (n + 7 < strInput.size()) ? 7
                                               : (strInput.size() - n));
    }
    Sha256Final(tCtx, byDigest);
    BURST_EXPECT_STR_EQ(cReport, strOneShot, Sha256Hex(byDigest));
}

/** @brief Run all SHA-256 tests. */
void RunSha256Tests(CTestReport& cReport)
{
    TestSha256Vectors(cReport);
    TestSha256Streaming(cReport);
}
