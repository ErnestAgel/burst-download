/**
 * @file toggle.cpp
 * @brief 模式切换 Switch 开关实现（§3.2/F1，iOS 风格拨杆）
 *
 * 布局：[文件下载] (●———) [视频下载]
 *   - 滑块在左 + 底槽灰 = 文件模式（OFF）；滑块在右 + 底槽主题色 = 视频模式（ON）
 *   - 点击整条任意处切换；滑块位置按 anim(0~1) 平滑滑动
 *   - 两端标签：激活侧亮色、非激活侧暗色
 *
 * 实现：整条 InvisibleButton（标准 ButtonBehavior 状态机）处理点击/激活，
 * ImDrawList 按绝对坐标绘制；仅用 imgui.h 公开 API（不依赖 imgui_internal）。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "toggle.h"

#include <algorithm>
#include <cmath>

bool ToggleMode(const char* left, const char* right, bool& videoMode,
                float width) {
    (void)width; /* 自适应内容宽度（旧 width 参数保留兼容） */

    /* 开关尺寸（iOS 风格） */
    const float switch_w = 46.0f; /* 底槽宽 */
    const float switch_h = 24.0f; /* 底槽高 */
    const float gap = 10.0f;      /* 文字与开关间距 */

    const ImVec2 ls = ImGui::CalcTextSize(left);
    const ImVec2 rs = ImGui::CalcTextSize(right);
    const float total_w = ls.x + gap + switch_w + gap + rs.x;
    const float total_h = std::max(std::max(ls.y, rs.y), switch_h);
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    /* 点击：整条 InvisibleButton（ID 固定 "##toggle_switch"，语言切换不影响） */
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(
        "##toggle_switch", ImVec2(total_w, total_h));
    const bool hovered = ImGui::IsItemHovered(); /* 立即读取，防后续覆盖 LastItem */
    if (clicked) {
        videoMode = !videoMode;
    }

    /* 滑块动画：anim 0=左(文件) 1=右(视频)，~180ms 平滑滑动 */
    ImGuiID anim_id = ImGui::GetID("##toggle_anim");
    float& anim = *(float*)ImGui::GetStateStorage()->GetFloatRef(
        anim_id, videoMode ? 1.0f : 0.0f);
    const float target = videoMode ? 1.0f : 0.0f;
    if (anim != target) {
        anim += (target - anim) *
                std::min(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
        if (std::fabs(target - anim) < 0.005f) {
            anim = target;
        }
    }

    /* 各元素位置（整条垂直居中） */
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float cy = pos.y + total_h * 0.5f;
    const ImVec2 lpos(pos.x, cy - ls.y * 0.5f);
    const ImVec2 sw_min(pos.x + ls.x + gap, cy - switch_h * 0.5f);
    const ImVec2 sw_max(sw_min.x + switch_w, sw_min.y + switch_h);
    const ImVec2 rpos(sw_max.x + gap, cy - rs.y * 0.5f);

    /* 底槽：OFF 灰 / ON 主题强调色（CheckMark 在 One Dark 下为强调蓝） */
    const ImU32 on_col = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImU32 off_col = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 track_col = (anim >= 0.5f) ? on_col : off_col;
    const float radius = switch_h * 0.5f;
    draw->AddRectFilled(sw_min, sw_max, track_col, radius);
    /* hover 提示：底槽描边 */
    if (hovered) {
        draw->AddRect(sw_min, sw_max, ImGui::GetColorU32(ImGuiCol_Border),
                      radius, 0, 1.0f);
    }

    /* 圆形滑块（白色拨杆），按 anim 从左滑到右 */
    const float knob_d = switch_h - 6.0f;
    const float kx0 = sw_min.x + 3.0f + knob_d * 0.5f;
    const float kx1 = sw_max.x - 3.0f - knob_d * 0.5f;
    const float kx = kx0 + (kx1 - kx0) * anim;
    draw->AddCircleFilled(ImVec2(kx, cy), knob_d * 0.5f,
                          IM_COL32(255, 255, 255, 255));
    /* 滑块边缘（立体感） */
    draw->AddCircle(ImVec2(kx, cy), knob_d * 0.5f,
                    ImGui::GetColorU32(ImGuiCol_Border), 0, 1.0f);

    /* 标签文字：激活侧亮、非激活侧暗（视频模式 → 右侧亮） */
    const ImU32 text_on = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 text_off = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    draw->AddText(lpos, videoMode ? text_off : text_on, left);
    draw->AddText(rpos, videoMode ? text_on : text_off, right);

    return clicked;
}
