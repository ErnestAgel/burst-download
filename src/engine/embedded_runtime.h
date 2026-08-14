#ifndef EMBEDDED_RUNTIME_H
#define EMBEDDED_RUNTIME_H

#include <string>

/**
 * @brief Record the executable path for runtime asset location.
 */
void EmbedSetExePath(const std::string& exe_path);

/**
 * @brief Get the recorded executable path.
 */
std::string EmbedGetExePath();

/**
 * @brief Last extraction failure reason (empty when the last extraction
 *        succeeded); surfaced in task errors for diagnostics.
 */
std::string EmbedRuntimeLastError();

/**
 * @brief Ensure the runtime assets are ready and output their root dir.
 * @param home Output: ready runtime root (contains stdlib/); empty when
 *             unavailable.
 * @return TRUE on success (false means unavailable; the caller keeps
 *         falling back).
 * @note Reuses the cache when the version marker matches; rebuilds it
 *       automatically after cleanup, guarded by a cross-process lock.
 */
bool ExtractEmbeddedRuntime(std::string& home);

#endif  // EMBEDDED_RUNTIME_H
