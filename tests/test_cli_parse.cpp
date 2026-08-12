/**
 * @file test_cli_parse.cpp
 * @brief Unit tests for the pure CLI argument parser.
 *
 * The tests document the parser contract, including the explicit output-set
 * flag introduced by issue R11 (no "./test" sentinel anymore).
 */

#include <string>
#include <vector>

#include "cli_parse.h"
#include "test_framework.h"

/**
 * @brief Run the parser with a plain argument list.
 * @param vecArgs Arguments including the program name.
 * @param tOpts Output options.
 * @param bOk Output parse result.
 * @param strError Output error text.
 */
static void RunParse(const std::vector<std::string>& vecArgs,
                     TCliOptions& tOpts, BOOL32& bOk, std::string& strError)
{
    std::vector<char*> vecArgv;
    for (s32 nIndex = 0; nIndex < (s32)vecArgs.size(); ++nIndex)
    {
        vecArgv.push_back(const_cast<char*>(vecArgs[nIndex].c_str()));
    }
    bOk = CliParseArgs((s32)vecArgv.size(), vecArgv.data(), tOpts,
                       4, 8, strError);
}

/** @brief Test: no arguments is a usage error without an error message. */
static void TestCliParseNoArgs(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: no args");
    std::vector<std::string> vecArgs = {"burst"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
    BURST_EXPECT_TRUE(cReport, strError.empty());
}

/** @brief Test: -v / --version only at the first argument. */
static void TestCliParseVersion(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: version");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsShort = {"burst", "-v"};
    RunParse(vecArgsShort, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionVersion);

    std::vector<std::string> vecArgsLong = {"burst", "--version"};
    RunParse(vecArgsLong, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionVersion);
}

/** @brief Test: -h / --help first or mid-argument-list. */
static void TestCliParseHelp(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: help");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsFirst = {"burst", "-h"};
    RunParse(vecArgsFirst, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionHelp);

    std::vector<std::string> vecArgsMid = {"burst", "--video", "u", "--help"};
    RunParse(vecArgsMid, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionHelp);
}

/** @brief Test: plain download defaults. */
static void TestCliParseDownloadDefaults(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: download defaults");
    std::vector<std::string> vecArgs = {"burst", "https://example.com/a.iso"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionDownload);
    BURST_EXPECT_STR_EQ(cReport, "https://example.com/a.iso",
                        tOpts.vecUrls[0]);
    BURST_EXPECT_TRUE(cReport, tOpts.bOutputSet == FALSE);
    BURST_EXPECT_TRUE(cReport, tOpts.strFilename.empty());
    BURST_EXPECT_TRUE(cReport, tOpts.nThreads == 4);
    BURST_EXPECT_TRUE(cReport, tOpts.nTimeout == 60);
    BURST_EXPECT_TRUE(cReport, tOpts.bVideoMode == FALSE);
    BURST_EXPECT_TRUE(cReport, tOpts.bAutoUpdateParser == TRUE);
}

/** @brief Test: -o marks the output as explicitly set (issue R11). */
static void TestCliParseOutputOption(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: -o option");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsOut = {"burst", "u", "-o", "out.bin"};
    RunParse(vecArgsOut, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bOutputSet == TRUE);
    BURST_EXPECT_STR_EQ(cReport, "out.bin", tOpts.strFilename);

    std::vector<std::string> vecArgsExplicit =
        {"burst", "u", "-o", "./test"};
    RunParse(vecArgsExplicit, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bOutputSet == TRUE);
    BURST_EXPECT_STR_EQ(cReport, "./test", tOpts.strFilename);
}

/** @brief Test: -t thread clamping. */
static void TestCliParseThreads(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: -t clamp");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsLow = {"burst", "u", "-t", "0"};
    RunParse(vecArgsLow, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nThreads == 1);

    std::vector<std::string> vecArgsHigh = {"burst", "u", "-t", "99"};
    RunParse(vecArgsHigh, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nThreads == 8);

    std::vector<std::string> vecArgsNormal = {"burst", "u", "-t", "3"};
    RunParse(vecArgsNormal, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nThreads == 3);
}

/** @brief Test: --timeout / --no-timeout (including atoi fallback). */
static void TestCliParseTimeout(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: timeout");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsZero = {"burst", "u", "--timeout", "0"};
    RunParse(vecArgsZero, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nTimeout == 0);

    std::vector<std::string> vecArgsNo = {"burst", "u", "--no-timeout"};
    RunParse(vecArgsNo, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nTimeout == 0);

    std::vector<std::string> vecArgsText = {"burst", "u", "--timeout", "abc"};
    RunParse(vecArgsText, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nTimeout == 0);
}

/** @brief Test: --video, cookies, and --no-auto-update options. */
static void TestCliParseVideoOptions(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: video options");
    std::vector<std::string> vecArgs =
        {"burst", "--video", "https://bili/BV1", "-o", "movie",
         "--cookies-from-browser", "chrome", "--cookie", "SESSDATA=x",
         "--no-auto-update"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionDownload);
    BURST_EXPECT_TRUE(cReport, tOpts.bVideoMode == TRUE);
    BURST_EXPECT_STR_EQ(cReport, "https://bili/BV1", tOpts.strVideoUrl);
    BURST_EXPECT_STR_EQ(cReport, "movie", tOpts.strFilename);
    BURST_EXPECT_STR_EQ(cReport, "chrome", tOpts.strCookiesFromBrowser);
    BURST_EXPECT_STR_EQ(cReport, "SESSDATA=x", tOpts.strCookie);
    BURST_EXPECT_TRUE(cReport, tOpts.bAutoUpdateParser == FALSE);
}

/** @brief Test: --update-parser action. */
static void TestCliParseUpdateParser(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: update parser");
    std::vector<std::string> vecArgs = {"burst", "--update-parser"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.emAction == emCliActionUpdateParser);
}

/** @brief Test: --verify flag and its optional sha256 value. */
static void TestCliParseVerify(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: --verify");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsFlag = {"burst", "u", "--verify"};
    RunParse(vecArgsFlag, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bVerify == TRUE);

    std::vector<std::string> vecArgsValue =
        {"burst", "u", "--verify", "sha256"};
    RunParse(vecArgsValue, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bVerify == TRUE);

    std::vector<std::string> vecArgsBad =
        {"burst", "u", "--verify", "md5"};
    RunParse(vecArgsBad, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
}

/** @brief Test: --continue flag. */
static void TestCliParseContinue(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: --continue");
    std::vector<std::string> vecArgs = {"burst", "u", "-o", "f.bin",
                                        "--continue"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bContinue == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bOutputSet == TRUE);
}

/** @brief Test: --delete-partial flag. */
static void TestCliParseDeletePartial(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: --delete-partial");
    std::vector<std::string> vecArgs = {"burst", "u", "-o", "f.bin",
                                        "--delete-partial"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.bDeletePartial == TRUE);
}

/** @brief Test: unknown options and malformed value pairs. */
static void TestCliParseErrors(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: errors");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgsUnknown = {"burst", "--bogus"};
    RunParse(vecArgsUnknown, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
    BURST_EXPECT_TRUE(cReport, strError.find("unknown argument") !=
                               std::string::npos);

    std::vector<std::string> vecArgsMissingJobs = {"burst", "u", "-j"};
    RunParse(vecArgsMissingJobs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);

    std::vector<std::string> vecArgsMissingOutput = {"burst", "u", "-o"};
    RunParse(vecArgsMissingOutput, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
    BURST_EXPECT_TRUE(cReport, strError.find("-o") != std::string::npos);

    std::vector<std::string> vecArgsMissingVideo = {"burst", "--video"};
    RunParse(vecArgsMissingVideo, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
    BURST_EXPECT_TRUE(cReport, strError.find("--video") !=
                                std::string::npos);

    std::vector<std::string> vecArgsDashValue = {"burst", "u", "-t", "-1"};
    RunParse(vecArgsDashValue, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
}

/** @brief Test: --version is rejected when not the first argument. */
static void TestCliParseVersionMidList(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: version mid list");
    std::vector<std::string> vecArgs = {"burst", "u", "--version"};
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == FALSE);
}

/** @brief Test: multiple positional URLs and the -j jobs option. */
static void TestCliParseMultiUrl(CTestReport& cReport)
{
    cReport.BeginCase("cli_parse: multi URL + -j");
    TCliOptions tOpts = {};
    std::string strError;
    BOOL32 bOk = FALSE;

    std::vector<std::string> vecArgs = {"burst", "https://a/f1.iso",
                                        "https://b/f2.iso", "-j", "4"};
    RunParse(vecArgs, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.vecUrls.size() == 2u);
    if (tOpts.vecUrls.size() == 2u)
    {
        BURST_EXPECT_STR_EQ(cReport, "https://a/f1.iso",
                            tOpts.vecUrls[0]);
        BURST_EXPECT_STR_EQ(cReport, "https://b/f2.iso",
                            tOpts.vecUrls[1]);
    }
    BURST_EXPECT_TRUE(cReport, tOpts.nJobs == 4);

    /* -j clamps to [1, 8]. */
    std::vector<std::string> vecArgsClamp = {"burst", "u", "-j", "99"};
    RunParse(vecArgsClamp, tOpts, bOk, strError);
    BURST_EXPECT_TRUE(cReport, bOk == TRUE);
    BURST_EXPECT_TRUE(cReport, tOpts.nJobs == 8);
}

/** @brief Run all CLI parser tests. */
void RunCliParseTests(CTestReport& cReport)
{
    TestCliParseNoArgs(cReport);
    TestCliParseVersion(cReport);
    TestCliParseHelp(cReport);
    TestCliParseDownloadDefaults(cReport);
    TestCliParseOutputOption(cReport);
    TestCliParseThreads(cReport);
    TestCliParseTimeout(cReport);
    TestCliParseVideoOptions(cReport);
    TestCliParseUpdateParser(cReport);
    TestCliParseVerify(cReport);
    TestCliParseContinue(cReport);
    TestCliParseDeletePartial(cReport);
    TestCliParseErrors(cReport);
    TestCliParseVersionMidList(cReport);
    TestCliParseMultiUrl(cReport);
}
