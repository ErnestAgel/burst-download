#ifndef BURST_CLI_PARSE_H
#define BURST_CLI_PARSE_H

/**
 * @file cli_parse.h
 * @brief Pure command-line argument parsing for the Burst CLI entry.
 *
 * The parser owns no global state and performs no I/O except reading the
 * argument vector, so it can be unit tested without curl/Python/FFmpeg.
 */

#include <string>
#include <vector>
#include "burst_types.h"

/** @brief Parsed CLI action. */
typedef enum tagCliAction
{
    emCliActionNone = 0,
    emCliActionVersion,
    emCliActionHelp,
    emCliActionUpdateParser,
    emCliActionDownload
} TCliAction;

/** @brief Parsed CLI options (all fields valid after successful parse). */
typedef struct tagCliOptions
{
    TCliAction  emAction;
    std::vector<std::string> vecUrls;
    std::string strVideoUrl;
    std::string strFilename;
    BOOL32      bOutputSet;
    BOOL32      bVerify;
    BOOL32      bContinue;
    BOOL32      bDeletePartial;
    s32         nThreads;
    s32         nTimeout;
    s32         nJobs;
    BOOL32      bVideoMode;
    std::string strCookiesFromBrowser;
    std::string strCookie;
} TCliOptions;

/**
 * @brief Parse CLI arguments into options.
 * @param nArgc Argument count (including program name).
 * @param ppszArgv Argument vector (including program name).
 * @param tOpts Output options; fully initialized on success.
 * @param nDefaultThreads Default thread count injected by the caller.
 * @param nMaxThreads Upper clamp for the -t option.
 * @param strError Error message on failure (empty when argc < 2).
 * @return TRUE on success, FALSE on usage error.
 */
BOOL32 CliParseArgs(s32 nArgc, char** ppszArgv, TCliOptions& tOpts,
                    s32 nDefaultThreads, s32 nMaxThreads,
                    std::string& strError);

#endif  // BURST_CLI_PARSE_H
