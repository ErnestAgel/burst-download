/**
 * @file test_pathutil.cpp
 * @brief Unit tests for URL base-name derivation.
 *
 * UrlBaseName tests document the raw last-segment derivation; SanitizeFileName
 * tests cover the safe-name layer added for issue S3.
 */

#include <string>

#include "pathutil.h"
#include "test_framework.h"

/** @brief Test: plain URL path segments. */
static void TestPathUtilPlain(CTestReport& cReport)
{
    cReport.BeginCase("pathutil: plain segments");
    BURST_EXPECT_STR_EQ(cReport, "file.iso",
                        UrlBaseName("https://host/a/file.iso"));
    BURST_EXPECT_STR_EQ(cReport, "file.iso",
                        UrlBaseName("https://host/a/file.iso?x=1#frag"));
    BURST_EXPECT_STR_EQ(cReport, "file",
                        UrlBaseName("https://host/a/file"));
}

/** @brief Test: trailing slashes and empty tail. */
static void TestPathUtilTrailingSlash(CTestReport& cReport)
{
    cReport.BeginCase("pathutil: trailing slash");
    BURST_EXPECT_STR_EQ(cReport, "a",
                        UrlBaseName("https://host/a/"));
    BURST_EXPECT_STR_EQ(cReport, "a",
                        UrlBaseName("https://host/a//"));
}

/** @brief Test: current unsafe behavior for dot segments (documents S3). */
static void TestPathUtilDotSegments(CTestReport& cReport)
{
    cReport.BeginCase("pathutil: raw dot segments");
    BURST_EXPECT_STR_EQ(cReport, "evil",
                        UrlBaseName("https://host/a/..\\evil"));
    BURST_EXPECT_STR_EQ(cReport, "..",
                        UrlBaseName("https://host/dir/.."));
    BURST_EXPECT_STR_EQ(cReport, "..",
                        UrlBaseName("https://host/a/..\\..\\.."));
}

/** @brief Test: plain file names pass through unchanged. */
static void TestSanitizePlain(CTestReport& cReport)
{
    cReport.BeginCase("sanitize: plain names");
    BURST_EXPECT_STR_EQ(cReport, "file.iso", SanitizeFileName("file.iso"));
    BURST_EXPECT_STR_EQ(cReport, "a b.txt", SanitizeFileName("a b.txt"));
    BURST_EXPECT_STR_EQ(cReport, "中文名.bin", SanitizeFileName("中文名.bin"));
}

/** @brief Test: path separators and illegal characters are replaced. */
static void TestSanitizeIllegalChars(CTestReport& cReport)
{
    cReport.BeginCase("sanitize: illegal chars");
    BURST_EXPECT_STR_EQ(cReport, "a_b_c", SanitizeFileName("a/b\\c"));
    BURST_EXPECT_STR_EQ(cReport, "a_b_c_d_e_f_g",
                        SanitizeFileName("a<b>c:d|e?f*g"));
    BURST_EXPECT_STR_EQ(cReport, "a_b", SanitizeFileName("a%2Fb"));
    BURST_EXPECT_STR_EQ(cReport, "trail", SanitizeFileName("trail. "));
}

/** @brief Test: dot/empty names are rejected. */
static void TestSanitizeRejects(CTestReport& cReport)
{
    cReport.BeginCase("sanitize: rejects");
    BURST_EXPECT_TRUE(cReport, SanitizeFileName("").empty());
    BURST_EXPECT_TRUE(cReport, SanitizeFileName(".").empty());
    BURST_EXPECT_TRUE(cReport, SanitizeFileName("..").empty());
    BURST_EXPECT_TRUE(cReport, SanitizeFileName("%2e%2e").empty());
}

/** @brief Test: Windows reserved device names are neutralized. */
static void TestSanitizeReservedNames(CTestReport& cReport)
{
    cReport.BeginCase("sanitize: reserved names");
    BURST_EXPECT_STR_EQ(cReport, "_CON", SanitizeFileName("CON"));
    BURST_EXPECT_STR_EQ(cReport, "_con.txt", SanitizeFileName("con.txt"));
    BURST_EXPECT_STR_EQ(cReport, "_nul", SanitizeFileName("nul"));
    BURST_EXPECT_STR_EQ(cReport, "_LPT1", SanitizeFileName("LPT1"));
    BURST_EXPECT_STR_EQ(cReport, "COM10", SanitizeFileName("COM10"));
}

/** @brief Test: overlong names are clamped. */
static void TestSanitizeLongName(CTestReport& cReport)
{
    cReport.BeginCase("sanitize: long name");
    const std::string strLong(300, 'a');
    const std::string strOut = SanitizeFileName(strLong);
    BURST_EXPECT_TRUE(cReport, strOut.size() == 200);
}

/** @brief Run all path-util tests. */
void RunPathUtilTests(CTestReport& cReport)
{
    TestPathUtilPlain(cReport);
    TestPathUtilTrailingSlash(cReport);
    TestPathUtilDotSegments(cReport);
    TestSanitizePlain(cReport);
    TestSanitizeIllegalChars(cReport);
    TestSanitizeRejects(cReport);
    TestSanitizeReservedNames(cReport);
    TestSanitizeLongName(cReport);
}
