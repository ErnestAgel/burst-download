/**
 * @file dialogs.cpp
 * @brief 模态弹窗实现（见 dialogs.h）
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "dialogs.h"

#include "i18n.h"
#include "imgui.h"

namespace dialogs {

namespace {

/** @brief 每帧按 open 状态打开 popup，再渲染模态体 */
bool BeginModal(const char* id, bool& open) {
    if (open) {
        ImGui::OpenPopup(id);
    }
    if (!ImGui::BeginPopupModal(id, &open,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }
    return true;
}

void EndModal(bool& open) {
    ImGui::EndPopup();
    (void)open;
}

}  // namespace

void ShowError(const std::string& title, const std::string& message,
               const std::string& guide, bool& open) {
    if (!BeginModal("##err", open)) {
        return;
    }
    ImGui::TextWrapped("%s", title.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("%s", message.c_str());
    if (!guide.empty()) {
        ImGui::TextWrapped("%s", guide.c_str());
    }
    ImGui::Separator();
    if (ImGui::Button(i18n::T("dialog.error.ok"), ImVec2(120, 0))) {
        open = false;
        ImGui::CloseCurrentPopup();
    }
    EndModal(open);
}

ExistsChoice ShowFileExists(const std::string& path, bool& open) {
    ExistsChoice choice = ExistsChoice::None;
    if (!BeginModal("##exists", open)) {
        return choice;
    }
    ImGui::TextWrapped("%s", i18n::T("dialog.exists.title"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", path.c_str());
    ImGui::TextWrapped("%s", i18n::T("dialog.exists.prompt"));
    ImGui::Separator();

    auto Btn = [&](const char* key, ExistsChoice c) {
        if (ImGui::Button(i18n::T(key), ImVec2(110, 0))) {
            choice = c;
            open = false;
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

    EndModal(open);
    return choice;
}

void ShowDone(const std::string& path, bool& open) {
    if (!BeginModal("##done", open)) {
        return;
    }
    ImGui::TextWrapped("%s", i18n::T("dialog.done.title"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", path.c_str());
    ImGui::Separator();
    if (ImGui::Button(i18n::T("dialog.done.ok"), ImVec2(120, 0))) {
        open = false;
        ImGui::CloseCurrentPopup();
    }
    EndModal(open);
}

}  // namespace dialogs
