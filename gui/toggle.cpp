/**
 * @file toggle.cpp
 * @brief 双向拨动开关实现（§3.2）
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "toggle.h"

#include "imgui_internal.h"  /* ImGuiWindow/GImGui/ImRect/ButtonBehavior 等内部 API */

bool ToggleMode(const char* left, const char* right, bool& videoMode,
                float width) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) {
        return false;
    }
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    /* 尺寸：高度按文本行高 + 内边距 */
    const float height = ImGui::GetFrameHeight() + 4.0f;
    const ImVec2 size(width, height);
    const ImRect bb(window->DC.CursorPos,
                    ImVec2(window->DC.CursorPos.x + size.x,
                           window->DC.CursorPos.y + size.y));
    ImGui::ItemSize(size, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, 0)) {
        return false;
    }

    /* 命中区域（InvisibleButton 等价物） */
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, ImGui::GetID(left), &hovered,
                                         &held, ImGuiButtonFlags_None);

    /* 点击切换模式 */
    bool changed = false;
    if (pressed) {
        videoMode = !videoMode;
        changed = true;
    }

    /* 滑块动画：0.0=左侧(文件)，1.0=右侧(视频) */
    ImGuiID anim_id = ImGui::GetID("##toggle_anim");
    float& anim = *(float*)ImGui::GetStateStorage()->GetFloatRef(
        anim_id, videoMode ? 1.0f : 0.0f);
    const float target = videoMode ? 1.0f : 0.0f;
    if (anim != target) {
        float speed = 1.0f / ImMax(1.0f, 1.0f / 60.0f) * 0.18f; /* ~180ms 滑动 */
        anim += (target - anim) * ImMin(1.0f, g.IO.DeltaTime * speed);
        if (ImFabs(target - anim) < 0.005f) {
            anim = target;
        }
    }

    /* 绘制 */
    ImDrawList* draw = window->DrawList;
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 bg_hover = ImGui::GetColorU32(ImGuiCol_FrameBgHovered);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const float radius = height * 0.5f;

    /* 底槽（圆角矩形） */
    draw->AddRectFilled(bb.Min, bb.Max, hovered ? bg_hover : bg, radius);

    /* 激活侧高亮：左侧文件(anim<0.5) 或 右侧视频(anim>=0.5) */
    const float mid_x = bb.Min.x + width * 0.5f;
    if (anim < 0.5f) {
        draw->AddRectFilled(bb.Min, ImVec2(mid_x, bb.Max.y), accent, radius,
                            ImDrawFlags_None);
        /* 右侧被覆盖部分无需圆角补齐：用直角矩形盖住衔接处 */
        draw->AddRectFilled(ImVec2(mid_x - radius, bb.Min.y),
                            ImVec2(mid_x, bb.Max.y), accent);
    } else {
        draw->AddRectFilled(ImVec2(mid_x, bb.Min.y), bb.Max, accent, radius,
                            ImDrawFlags_None);
        draw->AddRectFilled(ImVec2(mid_x, bb.Min.y),
                            ImVec2(mid_x + radius, bb.Max.y), accent);
    }

    /* 标签文本（滑块高亮已表达激活状态，不做激活侧变色） */
    ImVec2 ls = ImGui::CalcTextSize(left);
    ImVec2 rs = ImGui::CalcTextSize(right);
    ImVec2 lpos(bb.Min.x + (width * 0.5f - ls.x) * 0.5f,
                bb.Min.y + (height - ls.y) * 0.5f);
    ImVec2 rpos(bb.Min.x + width * 0.5f + (width * 0.5f - rs.x) * 0.5f,
                bb.Min.y + (height - rs.y) * 0.5f);
    draw->AddText(lpos, text, left);
    draw->AddText(rpos, text, right);

    /* 滑动圆钮（按 anim 位置，圆钮覆盖在激活侧） */
    const float knob_d = height - 8.0f;
    const float x0 = bb.Min.x + 4.0f;
    const float x1 = bb.Max.x - 4.0f - knob_d;
    const float kx = x0 + (x1 - x0) * anim;
    const float ky = bb.Min.y + (height - knob_d) * 0.5f;
    draw->AddCircleFilled(ImVec2(kx + knob_d * 0.5f, ky + knob_d * 0.5f),
                          knob_d * 0.5f,
                          ImGui::GetColorU32(ImGuiCol_SeparatorHovered));

    return changed;
}
