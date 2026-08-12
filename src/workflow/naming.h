#ifndef BURST_WORKFLOW_NAMING_H
#define BURST_WORKFLOW_NAMING_H

/**
 * @file naming.h
 * @brief CLI naming / overwrite-protection helpers (P9 workflow module).
 *
 * Extracted from the former main.cpp so the entry point only parses
 * arguments and the workflow module owns file naming decisions.
 */

#include <string>

/** @brief Whether a file already exists. */
bool FileExists(const std::string& strPath);

/** @brief Whether any video-mode output already exists. */
bool VideoOutputExists(const std::string& strBasename);

/** @brief Current local timestamp string (YYYYMMDD_HHMMSS). */
std::string CurrentTimeStamp();

/** @brief Append a timestamp before the extension: file.iso -> file_ts.iso. */
std::string StampName(const std::string& strBase);

#endif  // BURST_WORKFLOW_NAMING_H
