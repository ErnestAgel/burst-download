/**
 * @file cli_parse.cpp
 * @brief Pure CLI argument parser implementation.
 *
 * The parser mirrors the historical argument loop of RunCli exactly
 * (including the "./test" output sentinel), so extracting it is a
 * behavior-preserving refactor that makes parsing unit-testable.
 */

#include "cli_parse.h"

#include <cstdlib>
#include <cstring>

/**
 * @brief Check whether the argument after nIndex is a usable option value.
 * @param nArgc Argument count.
 * @param nIndex Current option index.
 * @param ppszArgv Argument vector.
 * @return TRUE when a value exists and does not start with '-'.
 */
static BOOL32 NextArgIsValue(s32 nArgc, s32 nIndex, char** ppszArgv)
{
    if (nIndex + 1 >= nArgc)
    {
        return FALSE;
    }
    if (ppszArgv[nIndex + 1][0] == '-')
    {
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Initialize options to their built-in defaults.
 * @param tOpts Options to initialize.
 * @param nDefaultThreads Default thread count.
 */
static void InitDefaultOptions(TCliOptions& tOpts, s32 nDefaultThreads)
{
    tOpts.emAction = emCliActionNone;
    tOpts.strUrl = "";
    tOpts.strVideoUrl = "";
    tOpts.strFilename = BURST_CLI_DEFAULT_FILENAME;
    tOpts.nThreads = nDefaultThreads;
    tOpts.nTimeout = 60;
    tOpts.bVideoMode = FALSE;
    tOpts.bAutoUpdateParser = TRUE;
    tOpts.strCookiesFromBrowser = "";
    tOpts.strCookie = "";
}

BOOL32 CliParseArgs(s32 nArgc, char** ppszArgv, TCliOptions& tOpts,
                    s32 nDefaultThreads, s32 nMaxThreads,
                    std::string& strError)
{
    strError = "";
    InitDefaultOptions(tOpts, nDefaultThreads);

    if (nArgc < 2)
    {
        return FALSE;  /* RunCli prints usage without an extra message */
    }
    if ((std::strcmp(ppszArgv[1], "-v") == 0) ||
        (std::strcmp(ppszArgv[1], "--version") == 0))
    {
        tOpts.emAction = emCliActionVersion;
        return TRUE;
    }
    if ((std::strcmp(ppszArgv[1], "-h") == 0) ||
        (std::strcmp(ppszArgv[1], "--help") == 0))
    {
        tOpts.emAction = emCliActionHelp;
        return TRUE;
    }

    tOpts.emAction = emCliActionDownload;
    for (s32 nIndex = 1; nIndex < nArgc; ++nIndex)
    {
        char* pszArg = ppszArgv[nIndex];
        if ((std::strcmp(pszArg, "--video") == 0) &&
            NextArgIsValue(nArgc, nIndex, ppszArgv))
        {
            tOpts.bVideoMode = TRUE;
            tOpts.strVideoUrl = ppszArgv[nIndex + 1];
            ++nIndex;
        }
        else if ((std::strcmp(pszArg, "-o") == 0) &&
                 NextArgIsValue(nArgc, nIndex, ppszArgv))
        {
            tOpts.strFilename = ppszArgv[nIndex + 1];
            ++nIndex;
        }
        else if ((std::strcmp(pszArg, "-t") == 0) &&
                 NextArgIsValue(nArgc, nIndex, ppszArgv))
        {
            tOpts.nThreads = std::atoi(ppszArgv[nIndex + 1]);
            if (tOpts.nThreads < 1)
            {
                tOpts.nThreads = 1;
            }
            if (tOpts.nThreads > nMaxThreads)
            {
                tOpts.nThreads = nMaxThreads;
            }
            ++nIndex;
        }
        else if ((std::strcmp(pszArg, "--timeout") == 0) &&
                 NextArgIsValue(nArgc, nIndex, ppszArgv))
        {
            tOpts.nTimeout = std::atoi(ppszArgv[nIndex + 1]);
            ++nIndex;
        }
        else if (std::strcmp(pszArg, "--no-timeout") == 0)
        {
            tOpts.nTimeout = 0;
        }
        else if (std::strcmp(pszArg, "--update-parser") == 0)
        {
            tOpts.emAction = emCliActionUpdateParser;
            return TRUE;
        }
        else if (std::strcmp(pszArg, "--no-auto-update") == 0)
        {
            tOpts.bAutoUpdateParser = FALSE;
        }
        else if ((std::strcmp(pszArg, "--cookies-from-browser") == 0) &&
                 NextArgIsValue(nArgc, nIndex, ppszArgv))
        {
            tOpts.strCookiesFromBrowser = ppszArgv[nIndex + 1];
            ++nIndex;
        }
        else if ((std::strcmp(pszArg, "--cookie") == 0) &&
                 NextArgIsValue(nArgc, nIndex, ppszArgv))
        {
            tOpts.strCookie = ppszArgv[nIndex + 1];
            ++nIndex;
        }
        else if ((std::strcmp(pszArg, "-h") == 0) ||
                 (std::strcmp(pszArg, "--help") == 0))
        {
            tOpts.emAction = emCliActionHelp;
            return TRUE;
        }
        else if ((pszArg[0] != '-') && tOpts.strUrl.empty())
        {
            tOpts.strUrl = pszArg;
        }
        else
        {
            strError = "未知参数: ";
            strError += pszArg;
            return FALSE;
        }
    }
    return TRUE;
}
