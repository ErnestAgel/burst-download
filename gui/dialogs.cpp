/**
 * @file dialogs.cpp
 * @brief Modal dialog implementations (see dialogs.h).
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "dialogs.h"

#include <cstring>

#include "i18n.h"
#include "imgui.h"

namespace dialogs {

namespace {

/** @brief Open a popup every frame by its open flag, then render the modal
 *         body (forced center of the window). */
bool BeginModal(const char* id, bool& bOpen) {
    if (bOpen) {
        ImGui::OpenPopup(id);
    }
    /* Center every frame (Always): previously ImGuiCond_Appearing did not
     * work reliably because OpenPopup runs each frame, so the popup showed
     * at the default position near the window top. */
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, &bOpen,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }
    return true;
}

void EndModal(bool& bOpen) {
    ImGui::EndPopup();
    (void)bOpen;
}

}  // namespace

void ShowError(const std::string& strTitle, const std::string& strMessage,
               const std::string& strGuide, bool& bOpen,
               const std::string& strPartialPath,
               bool* pbDeleteRequested) {
    if (!BeginModal("##err", bOpen)) {
        return;
    }
    ImGui::TextWrapped("%s", strTitle.c_str());
    ImGui::Separator();
    /* Selectable/copyable: read-only multi-line input (supports Ctrl+C). */
    {
        char buf[8192];
        std::strncpy(buf, strMessage.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        ImGui::InputTextMultiline("##errmsg", buf, sizeof(buf),
                                  ImVec2(-FLT_MIN, 0.0f),
                                  ImGuiInputTextFlags_ReadOnly);
    }
    if (!strGuide.empty()) {
        ImGui::TextWrapped("%s", strGuide.c_str());
    }
    ImGui::Separator();
    if (ImGui::Button(i18n::T("dialog.error.copy"))) {
        ImGui::SetClipboardText(strMessage.c_str());
    }
    ImGui::SameLine();
    if (!strPartialPath.empty() && pbDeleteRequested != nullptr) {
        if (ImGui::Button(i18n::T("dialog.error.delete_partial"))) {
            *pbDeleteRequested = true;
            bOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(i18n::T("dialog.error.ok"), ImVec2(120, 0))) {
        bOpen = false;
        ImGui::CloseCurrentPopup();
    }
    EndModal(bOpen);
}

ExistsChoice ShowFileExists(const std::string& strPath, bool& bOpen) {
    ExistsChoice choice = ExistsChoice::None;
    if (!BeginModal("##exists", bOpen)) {
        return choice;
    }
    ImGui::TextWrapped("%s", i18n::T("dialog.exists.title"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", strPath.c_str());
    ImGui::TextWrapped("%s", i18n::T("dialog.exists.prompt"));
    ImGui::Separator();

    auto Btn = [&](const char* key, ExistsChoice c) {
        if (ImGui::Button(i18n::T(key), ImVec2(110, 0))) {
            choice = c;
            bOpen = false;
            ImGui::CloseCurrentPopup();
        }
    };
    Btn("dialog.exists.resume", ExistsChoice::Resume);
    ImGui::SameLine();
    Btn("dialog.exists.overwrite", ExistsChoice::Overwrite);
    ImGui::SameLine();
    Btn("dialog.exists.rename", ExistsChoice::Rename);
    ImGui::SameLine();
    Btn("dialog.exists.cancel", ExistsChoice::Cancel);

    EndModal(bOpen);
    return choice;
}

void ShowDone(const std::string& strPath, bool& bOpen) {
    if (!BeginModal("##done", bOpen)) {
        return;
    }
    ImGui::TextWrapped("%s", i18n::T("dialog.done.title"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", strPath.c_str());
    ImGui::Separator();
    if (ImGui::Button(i18n::T("dialog.done.ok"), ImVec2(120, 0))) {
        bOpen = false;
        ImGui::CloseCurrentPopup();
    }
    EndModal(bOpen);
}

void ShowAbout(const std::string& strVersion, bool& bOpen) {
    if (!BeginModal("##about", bOpen)) {
        return;
    }
    ImGui::TextWrapped("%s", i18n::T("dialog.about.title"));
    ImGui::Separator();
    ImGui::Text("burst %s (Burst Download)", strVersion.c_str());
    ImGui::Text("%s:", i18n::T("dialog.about.platform"));
    ImGui::SameLine();
#ifdef _WIN32
    ImGui::TextUnformatted("Windows x86_64");
#elif defined(__aarch64__)
    ImGui::TextUnformatted("Linux aarch64");
#else
    ImGui::TextUnformatted("Linux x86_64");
#endif
    ImGui::Text("%s: MIT License", i18n::T("dialog.about.license"));
    ImGui::TextWrapped("Copyright (c) 2026 ErnestAgel");
    ImGui::Separator();
    if (ImGui::Button(i18n::T("dialog.about.ok"), ImVec2(120, 0))) {
        bOpen = false;
        ImGui::CloseCurrentPopup();
    }
    EndModal(bOpen);
}

}  // namespace dialogs
