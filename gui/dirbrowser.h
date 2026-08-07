#pragma once

#include <string>

/*
 * Linux 内置目录浏览器（std::filesystem 实现，零外部依赖）。
 *
 * 背景：Windows 保存路径用原生 IFileDialog（_WIN32 条件编译）；Linux 不引入
 * GTK3/nativefiledialog 以保持项目零外部依赖原则（gui-design.md §3 技术选型），
 * 故自绘简易目录浏览器：模态窗口列出当前目录的子目录，单击进入、可返回上级，
 * 点"选择此目录"取当前路径。
 *
 * 立即模式状态机：open=true 时渲染模态窗口；dir 为当前浏览目录（进出时更新）。
 * 返回 true 表示本次调用中用户点击了"选择此目录"，此时 dir 即选中目录、
 * open 已被置 false；取消（按钮/右上角关闭）仅将 open 置 false，返回 false。
 */
bool DirBrowserRender(std::string& dir, bool& open);
