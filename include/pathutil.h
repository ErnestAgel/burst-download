#ifndef BURST_PATHUTIL_H
#define BURST_PATHUTIL_H

/**
 * @file pathutil.h
 * @brief Pure path/URL helpers shared by CLI and GUI code.
 */

#include <string>

/**
 * @brief Derive a base name from a URL path (query and fragment stripped).
 * @param strUrl Source URL.
 * @return Last path segment; empty string when nothing remains.
 * @note Current implementation does not sanitize ".." or reserved names;
 *       sanitization is tracked as issue S3.
 */
std::string UrlBaseName(const std::string& strUrl);

#endif  // BURST_PATHUTIL_H
