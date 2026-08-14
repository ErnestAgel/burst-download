/**
 * @file cli_main.cpp
 * @brief Slim CLI entry (P9): parse arguments, handle informational
 *        actions, then delegate the download to the workflow module.
 *
 * Behavior-preserving extraction from the former src/main.cpp.
 */

#include <cstdio>
#include <string>

#include "Ccurl.h"
#include "app.h"
#include "cli_parse.h"
#include "embed_python.h"
#include "version.h"
#include "workflow.h"

namespace
{

/**
 * @brief Print command-line usage.
 * @param pszProg Program name.
 */
void PrintUsage(const char* pszProg)
{
    printf("burst %s (Burst Download)\n", BURST_VERSION_STRING);
    printf("Usage: %s <url> [-o filename] [-t threads] [--timeout sec] "
           "[--no-timeout]\n", pszProg);
    printf("       %s --video <video-url> [-o basename] [-t threads] "
           "[--timeout sec]\n", pszProg);
    printf("  <url>          download URL\n");
    printf("  --video <url>  video mode: parse a video page URL (Bilibili / "
           "YouTube etc.),\n");
    printf("                 resolve media stream URLs and download them with "
           "the\n");
    printf("                 multi-threaded chunked downloader (built-in "
           "parser)\n");
    printf("  --cookies-from-browser <name>  video mode: read login cookies "
           "from a browser\n");
    printf("                 (chrome/firefox/edge etc.) for logged-in HD "
           "streams\n");
    printf("  --cookie <str> request Cookie (e.g. \"SESSDATA=xxx; "
           "bili_jct=xxx\"),\n");
    printf("                 applies to video streams and plain downloads\n");
    printf("  -o filename    output file name (default: URL-derived name + "
           "timestamp to\n");
    printf("                 avoid overwrite; --video uses it as the output "
           "base name)\n");
    printf("  -t threads     download threads 1~%d (default adapts to CPU "
           "cores: %d)\n",
           BurstMaxThreads(), BurstDefaultThreads());
    printf("  -j, --jobs N   batch concurrency for multiple URLs (default "
           "2, max 8)\n");
    printf("  --timeout N    abort after N seconds without progress (default "
           "60, 0 = unlimited)\n");
    printf("  --no-timeout   disable automatic timeout (same as --timeout "
           "0)\n");
    printf("  --update-parser  update the built-in video parser to the latest "
           "version (needs network)\n");
    printf("  --verify-runtime verify the embedded Python runtime offline and "
           "exit (0 = ok)\n");
    printf("  --verify [sha256] compute and print the SHA-256 digest of the "
           "downloaded file after completion\n");
    printf("  --continue   resume an existing file instead of renaming it "
           "when the target already exists\n");
    printf("  --delete-partial  delete the partial file and resume meta after "
           "a failed download\n");
    printf("  -h, --help     show this help\n");
    printf("  -v, --version  show version\n");
    printf("Examples:\n");
    printf("  %s https://example.com/file.iso -o file.iso -t 8 --timeout "
           "30\n", pszProg);
    printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie -t "
           "8\n", pszProg);
    printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie "
           "--cookies-from-browser chrome\n", pszProg);
    printf("  %s https://example.com/private.zip -o p.zip --cookie "
           "\"SESSDATA=xxx\"\n", pszProg);
    printf("  %s https://a/file1.iso https://b/file2.iso -j 2\n", pszProg);
    printf("Logs: timeout interruptions, failures and completions are "
           "written to download.log\n");
}

}  // namespace

int RunCli(int argc, char** argv)
{
    TCliOptions tOpts = {};
    std::string strError;
    if (!CliParseArgs((s32)argc, argv, tOpts, BurstDefaultThreads(),
                      BurstMaxThreads(), strError))
    {
        if (!strError.empty())
        {
            printf("%s\n", strError.c_str());
        }
        PrintUsage(argv[0]);
        return 1;
    }

    if (tOpts.emAction == emCliActionVersion)
    {
        printf("burst %s (Burst Download)\n", BURST_VERSION_STRING);
        printf("platform: ");
#ifdef _WIN32
        printf("Windows x86_64\n");
#elif defined(__aarch64__)
        printf("Linux aarch64\n");
#else
        printf("Linux x86_64\n");
#endif
        return 0;
    }
    if (tOpts.emAction == emCliActionHelp)
    {
        PrintUsage(argv[0]);
        return 0;
    }
    if (tOpts.emAction == emCliActionUpdateParser)
    {
        std::string strMsg;
        if (!EmbedUpdateParser(argv[0], strMsg))
        {
            printf("update failed: %s\n", strMsg.c_str());
            return 1;
        }
        printf("%s\n", strMsg.c_str());
        return 0;
    }
    if (tOpts.emAction == emCliActionVerifyRuntime)
    {
        std::string strMsg;
        if (!EmbedVerifyRuntime(strMsg))
        {
            printf("runtime verification failed: %s\n", strMsg.c_str());
            return 1;
        }
        printf("runtime verification passed: %s\n", strMsg.c_str());
        return 0;
    }

    /* Initialize the embedded Python runtime (locate order: assets/ next to
     * the exe, temp cache, CURLBOLT_PYHOME, compile-time source tree). */
    EmbedPythonInit();

    if ((tOpts.bVideoMode == FALSE) && tOpts.vecUrls.empty())
    {
        PrintUsage(argv[0]);
        return 1;
    }
    return RunWorkflow(tOpts);
}
