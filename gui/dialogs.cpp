/**
 * @file dialogs.cpp
 * @brief 模态弹窗实现（见 dialogs.h）
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

/** @brief 每帧按 open 状态打开 popup，再渲染模态体（强制居中于窗口中心） */
bool BeginModal(const char* id, bool& open) {
    if (open) {
        ImGui::OpenPopup(id);
    }
    /* 居中：每帧强制（Always）定位在窗口客户区中心。
     * 注：此前用 ImGuiCond_Appearing 未生效——每帧 OpenPopup 导致条件判断
     * 不可靠，弹窗仍出现在默认位置（UI 顶部附近） */
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
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
    /* 可选中/复制：只读多行输入框（支持 Ctrl+C） */
    {
        char buf[8192];
        std::strncpy(buf, message.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        ImGui::InputTextMultiline("##errmsg", buf, sizeof(buf),
                                  ImVec2(-FLT_MIN, 0.0f),
                                  ImGuiInputTextFlags_ReadOnly);
    }
    if (!guide.empty()) {
        ImGui::TextWrapped("%s", guide.c_str());
    }
    ImGui::Separator();
    if (ImGui::Button(i18n::T("dialog.error.copy"))) {
        ImGui::SetClipboardText(message.c_str());
    }
    ImGui::SameLine();
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

void ShowAbout(const std::string& version, bool& open) {
    if (!BeginModal("##about", open)) {
        return;
    }
    ImGui::TextWrapped("%s", i18n::T("dialog.about.title"));
    ImGui::Separator();
    ImGui::Text("burst %s (Burst Download)", version.c_str());
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
        open = false;
        ImGui::CloseCurrentPopup();
    }
    EndModal(open);
}

}  // namespace dialogs
