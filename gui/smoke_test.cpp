// curlbolt-gui Phase 0.4 冒烟测试:
// 最小 GLFW + ImGui + OpenGL3 空窗口, 嵌入 Noto Sans SC 子集字体, 验证:
//   1. GLFW 窗口创建与 OpenGL 3.3 上下文
//   2. ImGui 上下文 + 字体从内存加载(零外部文件)
//   3. 中文渲染(每帧绘制中文文本 + InputText 输入框, 供人工验证 IME)
//   4. 正常退出(帧数/时间上限自动关闭, 验证退出流程无残留)
// 结果写入 smoke_result.txt(无控制台窗口也可见)。
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "imgui.h"
#include "imgui_internal.h"  // ImFont::FindGlyph(字形存在性验证用)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "font_data.h"

static void WriteResult(const char* fmt, ...) {
    FILE* f = fopen("smoke_result.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

int main() {
    remove("smoke_result.txt");
    WriteResult("step=begin\n");

    // ---- GLFW ----
    if (!glfwInit()) {
        WriteResult("step=fail_glfw_init\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window =
        glfwCreateWindow(800, 600, "curlbolt-gui smoke (Phase 0.4)", NULL, NULL);
    if (!window) {
        WriteResult("step=fail_create_window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    WriteResult("step=window_ok size=%dx%d\n", 800, 600);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 从内存加载嵌入字体(FontDataOwnedByAtlas=false: 数据是静态数组, 不能 free)
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(
        (void*)third_party_fonts_NotoSansSC_subset_ttf,
        (int)third_party_fonts_NotoSansSC_subset_ttf_len, 18.0f, &cfg,
        io.Fonts->GetGlyphRangesChineseFull());
    if (!font) {
        WriteResult("step=fail_font_load\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    WriteResult("step=font_ok glyph_zh=%s glyph_en=%s\n",
                font->IsGlyphInFont(0x4E2D) ? "OK" : "MISSING",  // 中
                font->IsGlyphInFont(0x45) ? "OK" : "MISSING");   // E

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) ||
        !ImGui_ImplOpenGL3_Init("#version 130")) {
        WriteResult("step=fail_backend\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    WriteResult("step=backend_ok\n");

    // ---- 渲染循环(限帧数/时间, 冒烟自动退出) ----
    int frames = 0;
    const double start = glfwGetTime();
    static char input_buf[128] = u8"中文输入";
    while (!glfwWindowShouldClose(window) && frames < 600 &&
           glfwGetTime() - start < 8.0) {
        glfwPollEvents();
        /* 每帧清屏（防 resize 拖影，与 main_gui 一致） */
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(480, 200), ImGuiCond_FirstUseEver);
        ImGui::Begin(u8"冒烟测试 — Smoke Test");
        ImGui::Text(u8"中文渲染测试: 下载进度 42%% · 速度 3.2 MB/s · ETA 00:12");
        ImGui::Text(u8"English rendering: 42%% · 3.2 MB/s · ETA 00:12");
        ImGui::Text(u8"模式: 文件下载 / 视频下载 · 线程数上限 8");
        ImGui::InputText(u8"输入框(IME 测试)", input_buf, sizeof(input_buf));
        ImGui::Text(u8"已输入: %s", input_buf);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        frames++;
    }
    WriteResult("step=loop_ok frames=%d\n", frames);

    // ---- 清理与退出(验证无残留: 顺序 join 之外的部分) ----
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    WriteResult("step=exit_ok\n");
    return 0;
}
