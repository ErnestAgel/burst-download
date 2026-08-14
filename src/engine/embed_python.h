/**
 * @file embed_python.h
 * @brief In-process embedded CPython + yt_dlp video URL parsing (pure code
 *        calls, no external process).
 *
 * Runtime assets (stdlib/, yt_dlp/) load from python_home:
 *   - default <executable-dir>/assets/
 *   - compile-time macro PYTHON_RUNTIME_FALLBACK source tree (dev/debug)
 *
 * All Python execution runs on a single dedicated worker thread with the
 * GIL held (issue R5); parse/update calls are budgeted and cancel-aware
 * (issues R1/R6).
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#ifndef EMBED_PYTHON_H
#define EMBED_PYTHON_H

#include <atomic>
#include <string>
#include <vector>

/**
 * @brief Initialize the embedded CPython (idempotent, thread-safe).
 * @param python_home Runtime assets root; when empty tries: exe assets/ ->
 *        temp cache -> CURLBOLT_PYHOME -> compile-time macro.
 * @return TRUE when initialization succeeded.
 */
bool EmbedPythonInit(const std::string& python_home = "");

/**
 * @brief Last initialization failure reason (empty on success).
 */
std::string EmbedLastInitError();

/**
 * @brief Verify the embedded Python runtime end to end without network:
 *        initialize the interpreter and import every module the video
 *        parser needs (os/json/ssl/hashlib/socket/yt_dlp).
 * @param msg Output: success description (runtime home + yt_dlp version)
 *        or the failure reason.
 * @return TRUE when the runtime initializes and all imports succeed.
 * @note Intended for CI smoke tests on raw builds (CURLBOLT_PYHOME) and on
 *       packaged artifacts (embedded blob); exit code drives gating.
 */
bool EmbedVerifyRuntime(std::string& msg);

/**
 * @brief Parse a video page with the embedded yt_dlp into media stream
 *        URLs.
 * @param url Video page URL (e.g. Bilibili/YouTube video page).
 * @param urls Output: media stream URL list (DASH order: video, audio).
 * @param cookies_from_browser Browser cookie source (chrome/firefox/edge,
 *        may be empty).
 * @param cookie Manual cookie string (may be empty).
 * @param err Failure description on error.
 * @param pbCancel Optional cancel flag; aborting mid-parse returns FALSE
 *        with an empty err (caller maps to a cancel state).
 * @return TRUE on success.
 * @note EmbedPythonInit must succeed first.
 */
bool EmbedParseVideoUrls(const std::string& url,
                         std::vector<std::string>& urls,
                         const std::string& cookies_from_browser,
                         const std::string& cookie,
                         std::string& err,
                         const std::atomic<bool>* pbCancel = nullptr);

/**
 * @brief Update the built-in video parser (yt_dlp package) to the latest
 *        GitHub release.
 * @param exe_path Executable path used to locate the same-dir assets/.
 * @param msg Output: result description ("already latest" or version
 *        change; failure reason).
 * @return TRUE on success ("already latest" counts as success).
 * @note Needs network; replaces only the yt_dlp package directory
 *       (atomic replace with rollback).  Budgeted at 60s.
 */
bool EmbedUpdateParser(const std::string& exe_path, std::string& msg);

/**
 * @brief Auto-update the built-in video parser (called at video mode
 *        start).
 * @param msg Output: result description; empty when the 24h throttle skips.
 * @param pbCancel Optional cancel flag; when set, the check is skipped.
 * @return TRUE when a check completed (throttle skip also returns TRUE;
 *         runtime missing / network failure returns FALSE, callers may
 *         ignore).
 * @note Throttled to once per 24h per runtime dir; failures are silent and
 *       do not block parsing.  Needs network; replaces only the yt_dlp
 *       package directory (atomic replace with rollback).
 */
bool EmbedAutoUpdateParser(std::string& msg,
                           const std::atomic<bool>* pbCancel = nullptr);

/**
 * @brief Stop the dedicated Python worker thread without finalizing the
 *        interpreter (safe for process exit).
 * @param nTimeoutSec Bounded wait; on timeout the thread is detached so a
 *        stuck network job cannot block exit.
 */
void EmbedPythonStopWorker(long nTimeoutSec = 2);

/**
 * @brief Release the embedded CPython and resources (optional, before
 *        process exit).
 */
void EmbedPythonShutdown();

#endif  // EMBED_PYTHON_H
