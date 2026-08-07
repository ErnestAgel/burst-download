#include "dirbrowser.h"

#include "i18n.h"
#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

bool DirBrowserRender(std::string& dir, bool& open) {
    if (!open) return false;

    ImGui::SetNextWindowSize(ImVec2(500, 380), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("##dirbrowser", &open,
                      ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return false;
    }

    bool picked = false;

    /* 当前浏览目录（只读显示） */
    ImGui::TextUnformatted(dir.c_str());
    ImGui::Separator();

    /* 收集子目录：跳过隐藏项（. 开头）与无权限项 */
    std::vector<std::string> dirs;
    std::error_code ec;
    fs::directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_directory(ec2) && !ec2) continue;
        if (ec2) continue;
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        dirs.push_back(name);
    }
    std::sort(dirs.begin(), dirs.end());

    ImGui::BeginChild("##dirlist",
                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6));
    if (dir != "/" && ImGui::Selectable("../")) {
        dir = fs::path(dir).parent_path().string();
        if (dir.empty()) dir = "/";
    }
    if (dirs.empty()) {
        ImGui::TextDisabled("(%s)", i18n::T("dir.empty"));
    }
    for (const auto& d : dirs) {
        const std::string label = d + "/";
        if (ImGui::Selectable(label.c_str())) {
            dir = (fs::path(dir) / d).lexically_normal().string();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button(i18n::T("dir.select"))) {
        picked = true;
        open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::T("button.cancel"))) open = false;

    ImGui::End();
    return picked;
}
