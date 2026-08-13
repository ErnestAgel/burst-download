/**
 * @file dialogs.h
 * @brief Modal dialogs (error guide / file-exists four-choice / done):
 *        all text goes through i18n::T(); raw curl errors stay English.
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

namespace dialogs {

/** @brief File-exists dialog user choice. */
enum class ExistsChoice {
    None = 0,      /**< Nothing chosen yet (dialog still open) */
    Resume,        /**< Resume: continue (Ccurl auto-detects the local file) */
    Overwrite,     /**< Overwrite: delete the local file and redownload */
    Rename,        /**< Rename: append a timestamp (caller restarts with the
                    *  new path) */
    Cancel,        /**< Cancel: do not download */
};

/**
 * @brief Error dialog (modal): title + message + category guide + OK, plus
 *        an optional "Delete Partial File" action (issue R9).
 * @param strTitle Dialog title (already translated).
 * @param strMessage Error message.
 * @param strGuide Category guide text.
 * @param bOpen Dialog open flag (set true to open; false after a button).
 * @param strPartialPath Partial file path; non-empty shows the delete button.
 * @param pbDeleteRequested Output: set true when the user clicks delete.
 */
void ShowError(const std::string& strTitle, const std::string& strMessage,
               const std::string& strGuide, bool& bOpen,
               const std::string& strPartialPath = "",
               bool* pbDeleteRequested = nullptr);

/**
 * @brief File-exists four-choice dialog.
 * @param strPath Target path.
 * @param bOpen Dialog open flag.
 * @return User choice (ExistsChoice::None while open).
 */
ExistsChoice ShowFileExists(const std::string& strPath, bool& bOpen);

/**
 * @brief Done dialog (modal): download complete + OK.
 * @param strPath Output file path.
 * @param bOpen Dialog open flag.
 */
void ShowDone(const std::string& strPath, bool& bOpen);

}  // namespace dialogs
