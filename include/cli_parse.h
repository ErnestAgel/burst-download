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
#include "burst_types.h"

/** @brief Sentinel used for "no -o option given" (legacy behavior, see R11). */
#define BURST_CLI_DEFAULT_FILENAME "./test"

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
    std::string strUrl;
    std::string strVideoUrl;
    std::string strFilename;
    s32         nThreads;
    s32         nTimeout;
    BOOL32      bVideoMode;
    BOOL32      bAutoUpdateParser;
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
