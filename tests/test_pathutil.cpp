/**
 * @file test_pathutil.cpp
 * @brief Unit tests for URL base-name derivation.
 *
 * The unsafe expectations document issue S3 (no sanitization yet); P1 will
 * add a sanitizer and tighten these cases to reject ".." escapes.
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
    cReport.BeginCase("pathutil: dot segments (S3 risk)");
    BURST_EXPECT_STR_EQ(cReport, "evil",
                        UrlBaseName("https://host/a/..\\evil"));
    BURST_EXPECT_STR_EQ(cReport, "..",
                        UrlBaseName("https://host/dir/.."));
    BURST_EXPECT_STR_EQ(cReport, "..",
                        UrlBaseName("https://host/a/..\\..\\.."));
}

/** @brief Run all path-util tests. */
void RunPathUtilTests(CTestReport& cReport)
{
    TestPathUtilPlain(cReport);
    TestPathUtilTrailingSlash(cReport);
    TestPathUtilDotSegments(cReport);
}
