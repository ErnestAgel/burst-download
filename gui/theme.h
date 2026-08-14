/**
 * @file theme.h
 * @brief UI 主题：Atom One Dark（用户指定配色）
 *
 * 参考 Atom 编辑器 One Dark 配色方案：
 *   背景 #282C34 / 面板 #21252B / 边框 #3E4451 / 文本 #ABB2BF
 *   强调 #61AFEF(蓝) #98C379(绿) #E5C07B(黄) #E06C75(红) #C678DD(紫)
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

namespace theme {

/**
 * @brief 应用 Burst Download 深色主题到 ImGui（视觉规格附录 C）
 * @note 需在 ImGui::CreateContext() 之后调用；窗口标题栏/边框由 style 统一定制
 */
void ApplyBurst();

}  // namespace theme
