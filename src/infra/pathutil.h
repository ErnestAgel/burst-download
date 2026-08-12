#ifndef BURST_PATHUTIL_H
#define BURST_PATHUTIL_H

/**
 * @file pathutil.h
 * @brief Pure path/URL helpers shared by CLI and GUI code.
 */

#include <string>

#include "burst_types.h"

/**
 * @brief Derive a base name from a URL path (query and fragment stripped).
 * @param strUrl Source URL.
 * @return Last path segment; empty string when nothing remains.
 */
std::string UrlBaseName(const std::string& strUrl);

/**
 * @brief Sanitize a file name for safe local storage (issue S3).
 *
 * Percent-decodes the input, replaces path separators, control characters
 * and Windows-reserved characters with '_', rejects "." / "..", neutralizes
 * Windows device names (CON/NUL/COM1...) and clamps the length.
 *
 * @param strName Raw name (typically the last URL path segment).
 * @return Sanitized name; empty string when nothing usable remains
 *         (caller must fall back to a default name).
 */
std::string SanitizeFileName(const std::string& strName);

#endif  // BURST_PATHUTIL_H
