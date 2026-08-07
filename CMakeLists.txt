cmake_minimum_required(VERSION 3.10)


project("burst-download")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 默认构建类型：Debug（链接动态库，便于调试）
#              Release（链接静态库 libcurl.a，产出单文件可执行程序，用于发布）
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型 (Debug / Release)" FORCE)
endif()

# 项目自带头文件（跨平台）
set(CURL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CURL_INCLUDE_DIR})

file(GLOB SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_compile_options(-g)
add_executable(burst ${SRC})

# ---- 平台目录：third_party/<PLATFORM_DIR> ----
if(WIN32)
  set(PLATFORM_DIR windows-x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(PLATFORM_DIR linux-aarch64)
else()
  set(PLATFORM_DIR linux-x86_64)
endif()

# ---- Python 嵌入（进程内 yt_dlp 解析，静态 libpython，Debug/Release 共用） ----
set(PYTHON_EMBED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/python)
# include 平台优先（pyconfig.h 为平台 configure 生成）；无平台 include 时回退共享目录（x86）
if(EXISTS ${PYTHON_EMBED_DIR}/${PLATFORM_DIR}/include)
  include_directories(${PYTHON_EMBED_DIR}/${PLATFORM_DIR}/include)
else()
  include_directories(${PYTHON_EMBED_DIR}/include)
endif()
target_compile_definitions(burst PRIVATE
  PYTHON_RUNTIME_FALLBACK="${CMAKE_CURRENT_SOURCE_DIR}/third_party/python/runtime")
if(EXISTS ${PYTHON_EMBED_DIR}/${PLATFORM_DIR}/libpython3.11.a)
  target_link_libraries(burst ${PYTHON_EMBED_DIR}/${PLATFORM_DIR}/libpython3.11.a)
endif()
if(UNIX)
  # libpython 依赖：openssl/zlib 用 third_party 干净静态库
  # （系统 openssl 静态库带 jitterentropy/zstd 依赖，全静态链接会失败）
  set(PY_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static)
  set(_PY_LIBS ${PY_DEPS_DIR}/libssl.a ${PY_DEPS_DIR}/libcrypto.a ${PY_DEPS_DIR}/libz.a
               dl pthread m util)
  # lzma/bz2 静态库随仓库分发（third_party/<platform>/static/），不依赖系统开发包；
  # libpython3.11.a 的 _bz2module/_lzmamodule 需要 BZ2_*/lzma_* 符号，缺失将导致链接失败
  foreach(_pyx lzma bz2)
    if(EXISTS ${PY_DEPS_DIR}/lib${_pyx}.a)
      list(APPEND _PY_LIBS ${PY_DEPS_DIR}/lib${_pyx}.a)
    else()
      message(FATAL_ERROR
        "缺少仓库内静态依赖: ${PY_DEPS_DIR}/lib${_pyx}.a\n"
        "libpython3.11.a 需要 ${_pyx} 符号，请从 bz2/lzma 开发包补充该静态库到仓库后再构建")
    endif()
  endforeach()
  target_link_libraries(burst ${_PY_LIBS})
endif()

# ---- FFmpeg 嵌入（进程内 remux 合并音视频轨，静态库，Debug/Release 共用） ----
set(FFMPEG_EMBED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg)
# include 平台优先（avconfig.h 为平台 configure 生成）；无平台 include 时回退共享目录（x86）
if(EXISTS ${FFMPEG_EMBED_DIR}/${PLATFORM_DIR}/include)
  include_directories(${FFMPEG_EMBED_DIR}/${PLATFORM_DIR}/include)
else()
  include_directories(${FFMPEG_EMBED_DIR}/include)
endif()
set(FFMPEG_LIB_DIR ${FFMPEG_EMBED_DIR}/${PLATFORM_DIR})
if(EXISTS ${FFMPEG_LIB_DIR}/libavformat.a)
  message(STATUS "链接 FFmpeg 静态库: ${FFMPEG_LIB_DIR}")
  target_link_libraries(burst
    ${FFMPEG_LIB_DIR}/libavformat.a
    ${FFMPEG_LIB_DIR}/libavcodec.a
    ${FFMPEG_LIB_DIR}/libavutil.a)
endif()

# ==================== Release：静态链接（单文件，用于发布） ====================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS "Release 构建：静态链接 third_party/${PLATFORM_DIR}/libcurl.a")
  set(CURL_STATIC_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static/libcurl.a)

  if(WIN32)
    # Windows: Schannel 原生 TLS，无需 openssl
    add_compile_definitions(CURL_STATICLIB)
    target_link_libraries(burst ${CURL_STATIC_LIB} crypt32 ws2_32 bcrypt)
    set_target_properties(burst PROPERTIES
      LINK_FLAGS "-static-libgcc -static-libstdc++ -static")
  else()
    # Linux: libcurl.a + 静态 openssl/zlib
    target_link_libraries(burst ${CURL_STATIC_LIB})

    # 自动发现 third_party/<platform>/static/ 静态依赖（openssl + zlib），存在即自动链接
    set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR})
    set(THIRD_PARTY_STATIC_LIBS
      ${THIRD_PARTY_DIR}/static/libssl.a
      ${THIRD_PARTY_DIR}/static/libcrypto.a
      ${THIRD_PARTY_DIR}/static/libz.a)
    set(THIRD_PARTY_LIBS_FOUND TRUE)
    foreach(_lib ${THIRD_PARTY_STATIC_LIBS})
      if(NOT EXISTS ${_lib})
        set(THIRD_PARTY_LIBS_FOUND FALSE)
        break()
      endif()
    endforeach()
    if(THIRD_PARTY_LIBS_FOUND)
      message(STATUS "third_party 静态依赖: ${THIRD_PARTY_DIR}（自动链接）")
      target_link_libraries(burst ${THIRD_PARTY_STATIC_LIBS})
    else()
      # 回退：手动传参（兼容旧方式）
      set(OPENSSL_ROOT "" CACHE PATH "静态 openssl 安装目录（含 lib/libssl.a、lib/libcrypto.a）")
      if(OPENSSL_ROOT)
        target_link_libraries(burst
          ${OPENSSL_ROOT}/lib/libssl.a ${OPENSSL_ROOT}/lib/libcrypto.a)
      endif()
      # 其他静态依赖（zlib/zstd 等），由构建脚本按需传入
      set(CURL_STATIC_DEPS "" CACHE STRING "额外静态依赖库列表（空格分隔）")
      if(CURL_STATIC_DEPS)
        target_link_libraries(burst ${CURL_STATIC_DEPS})
      endif()
    endif()
    target_link_libraries(burst dl pthread)
    # 全静态单文件（-static），目标机无需任何 libcurl/openssl 动态库
    set_target_properties(burst PROPERTIES
      LINK_FLAGS "-static-libgcc -static-libstdc++ -static")
  endif()

# ==================== Debug：动态库（便于调试） ====================
else()
  message(STATUS "Debug 构建：动态链接 third_party/${PLATFORM_DIR} 动态库")
  if(WIN32)
    # Windows: 优先链接项目自带的 mingw 版 libcurl，缺失时回退系统 curl（vcpkg / 官方安装包）
    set(CURL_WIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/windows-x86_64/dynamic)
    if(EXISTS ${CURL_WIN_DIR}/libcurl.dll.a)
      set(CURL_TARGETS ${CURL_WIN_DIR}/libcurl.dll.a)
    else()
      find_package(CURL REQUIRED)
      set(CURL_TARGETS CURL::libcurl)
    endif()
    target_link_libraries(burst ${CURL_TARGETS})
    # MinGW 下链接 FFmpeg 静态库/系统 API 所需库（bcrypt 随机数、ws2_32 套接字）
    if(MINGW)
      target_link_libraries(burst bcrypt ws2_32)
    endif()
    # 运行时 libcurl-4.dll 需与可执行文件同目录（mingw import lib 记录的 DLL 名为 libcurl-4.dll）
    if(EXISTS ${CURL_WIN_DIR}/libcurl-4.dll)
      add_custom_command(TARGET burst POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CURL_WIN_DIR}/libcurl-4.dll $<TARGET_FILE_DIR:burst>)
    endif()
    # MSVC 链接 libcurl 所需的系统库
    if(MSVC)
      target_link_libraries(burst ws2_32 crypt32 wldap32 normaliz)
    endif()
  else()
    set(CURL_TARGETS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic/libcurl.so)
    target_link_libraries(burst pthread ${CURL_TARGETS})
    # FFmpeg 静态库的 mov demuxer 需要 zlib（uncompress 等），Debug 下也链接仓库静态 zlib
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static/libz.a)
      target_link_libraries(burst ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static/libz.a)
    endif()
    # 运行时 rpath：确保可执行文件加载项目 third_party/<platform>/dynamic/ 下的 libcurl.so.4，而非系统库
    set_target_properties(burst PROPERTIES
      BUILD_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic
      INSTALL_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic)
  endif()
endif()

# ==================== Windows：嵌入 Python 运行依赖 dll 随程序分发 ====================
# 内嵌 libpython（MinGW import lib）运行时依赖 python311.dll 及 libffi-8.dll、
# libssl-3.dll/libcrypto-3.dll/vcruntime 等，需与可执行文件同目录才能加载（Debug/Release 通用）。
# 通用函数：给任意 Windows 目标追加 Python 运行依赖 dll 复制（CLI 与 GUI 共用）
function(burst_copy_python_runtime_dlls TARGET)
  if(WIN32)
    file(GLOB PY_RUNTIME_DLLS
         ${CMAKE_CURRENT_SOURCE_DIR}/third_party/python/windows-x86_64/dll/*.dll)
    foreach(_dll ${PY_RUNTIME_DLLS})
      add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dll} $<TARGET_FILE_DIR:${TARGET}>)
    endforeach()
  endif()
endfunction()
burst_copy_python_runtime_dlls(burst)

# ==================== Windows：MinGW 运行时 dll 随程序分发 ====================
# 非全静态构建（Debug、GUI 目标等）依赖 libgcc_s_seh-1.dll / libstdc++-6.dll /
# libwinpthread-1.dll，随仓库分发（third_party/mingw64-runtime/），POST_BUILD 复制到 exe 目录。
# Release CLI 使用 -static 全静态链接，不依赖这些 DLL，但复制无害且统一。
# 通用函数：给任意 Windows 目标追加 MinGW 运行时 dll 复制
function(burst_copy_mingw_runtime_dlls TARGET)
  if(WIN32)
    file(GLOB MINGW_RT_DLLS
         ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mingw64-runtime/windows-x86_64/dll/*.dll)
    foreach(_dll ${MINGW_RT_DLLS})
      add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dll} $<TARGET_FILE_DIR:${TARGET}>)
    endforeach()
  endif()
endfunction()
burst_copy_mingw_runtime_dlls(burst)

# ==================== GUI（burst-gui 正式目标 + burst-gui-smoke 开发冒烟） ====================
# 依赖：Dear ImGui（third_party/imgui，MIT）+ GLFW（third_party/glfw，zlib）+ Noto Sans SC 子集字体
# burst-gui：Phase 1 MVP（文件模式），链接核心 Ccurl + curl；视频/解析模块 Phase 2 加入
option(BUILD_GUI "Build burst GUI" ON)
if(BUILD_GUI)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS   OFF CACHE BOOL "GLFW 静态编译" FORCE)
  add_subdirectory(third_party/glfw)

  set(IMGUI_SRC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp)

  # ---- 正式 GUI 目标（Phase 2：文件模式 + 视频模式闭环） ----
  add_executable(burst-gui WIN32
    gui/main_gui.cpp
    gui/ui.cpp
    gui/toggle.cpp
    gui/i18n.cpp
    gui/worker.cpp
    gui/dialogs.cpp
    gui/crashguard.cpp
    gui/theme.cpp
    gui/dirbrowser.cpp
    src/Ccurl.cpp
    src/download_video.cpp
    src/video.cpp
    src/embed_python.cpp
    src/avmerge.cpp
    ${IMGUI_SRC})
  target_include_directories(burst-gui PRIVATE
    third_party/imgui
    third_party/imgui/backends
    gui
    include
    src)  # download_video.h / progress.h（Phase 2 视频模式）
  target_compile_options(burst-gui PRIVATE
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
  target_link_libraries(burst-gui PRIVATE glfw)

  if(MINGW)
    target_link_libraries(burst-gui PRIVATE
      opengl32 gdi32 user32 shell32 advapi32 winmm ole32)
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(burst-gui PRIVATE ${OPENGL_LIBRARIES})
  endif()

  # curl 链接（与 burst 同策略：Release 静态 / Debug 动态）
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(GUI_CURL_STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static/libcurl.a)
    if(EXISTS ${GUI_CURL_STATIC})
      target_compile_definitions(burst-gui PRIVATE CURL_STATICLIB)
      if(WIN32)
        target_link_libraries(burst-gui PRIVATE
          ${GUI_CURL_STATIC} crypt32 ws2_32 bcrypt)
      else()
        target_link_libraries(burst-gui PRIVATE
          ${GUI_CURL_STATIC} dl pthread)
      endif()
    endif()
  else()
    if(WIN32)
      set(GUI_CURL_DYN ${CMAKE_CURRENT_SOURCE_DIR}/third_party/windows-x86_64/dynamic)
      if(EXISTS ${GUI_CURL_DYN}/libcurl.dll.a)
        target_link_libraries(burst-gui PRIVATE
          ${GUI_CURL_DYN}/libcurl.dll.a bcrypt ws2_32)
        if(EXISTS ${GUI_CURL_DYN}/libcurl-4.dll)
          add_custom_command(TARGET burst-gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${GUI_CURL_DYN}/libcurl-4.dll $<TARGET_FILE_DIR:burst-gui>)
        endif()
      endif()
    else()
      target_link_libraries(burst-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic/libcurl.so pthread)
      set_target_properties(burst-gui PROPERTIES
        BUILD_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic)
    endif()
  endif()
  burst_copy_mingw_runtime_dlls(burst-gui)

  # ---- 视频模式（Phase 2）：Python 嵌入（解析）+ FFmpeg（合并）链接 ----
  # 与 CLI 同策略：运行时资源 fallback 到源码树 third_party/python/runtime（开发构建）；
  # 发布时 exe 同目录 python_runtime/ 优先（LocateRuntimeHome 探测顺序，见 embed_python.cpp）
  target_compile_definitions(burst-gui PRIVATE
    PYTHON_RUNTIME_FALLBACK="${CMAKE_CURRENT_SOURCE_DIR}/third_party/python/runtime")
  if(EXISTS ${PYTHON_EMBED_DIR}/${PLATFORM_DIR}/libpython3.11.a)
    target_link_libraries(burst-gui PRIVATE
      ${PYTHON_EMBED_DIR}/${PLATFORM_DIR}/libpython3.11.a)
  endif()
  if(UNIX)
    # libpython 依赖：openssl/zlib 用 third_party 干净静态库（同 CLI，见上）
    set(_PY_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static)
    target_link_libraries(burst-gui PRIVATE
      ${_PY_DEPS_DIR}/libssl.a ${_PY_DEPS_DIR}/libcrypto.a ${_PY_DEPS_DIR}/libz.a
      dl pthread m util)
    foreach(_pyx lzma bz2)
      if(EXISTS ${_PY_DEPS_DIR}/lib${_pyx}.a)
        target_link_libraries(burst-gui PRIVATE ${_PY_DEPS_DIR}/lib${_pyx}.a)
      endif()
    endforeach()
  endif()
  # FFmpeg 静态库（进程内 remux 合并音视频轨，同 CLI）
  if(EXISTS ${FFMPEG_LIB_DIR}/libavformat.a)
    target_link_libraries(burst-gui PRIVATE
      ${FFMPEG_LIB_DIR}/libavformat.a
      ${FFMPEG_LIB_DIR}/libavcodec.a
      ${FFMPEG_LIB_DIR}/libavutil.a)
    if(WIN32)
      # avutil 随机数种子依赖 bcrypt/ws2_32；须置于 ffmpeg 静态库之后（链接顺序）
      target_link_libraries(burst-gui PRIVATE bcrypt ws2_32)
    endif()
  endif()
  # Windows：Python 运行依赖 dll 随 GUI 分发（exe 同目录，7 个 dll，POST_BUILD 自动复制）
  burst_copy_python_runtime_dlls(burst-gui)

  # ---- 开发冒烟目标（Phase 0 验证窗口/字体链路，仅开发用） ----
  add_executable(burst-gui-smoke WIN32
    gui/smoke_test.cpp ${IMGUI_SRC})
  target_include_directories(burst-gui-smoke PRIVATE
    third_party/imgui
    third_party/imgui/backends
    gui)                                  # font_data.h
  target_compile_options(burst-gui-smoke PRIVATE
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
  target_link_libraries(burst-gui-smoke PRIVATE glfw)
  if(MINGW)
    target_link_libraries(burst-gui-smoke PRIVATE
      opengl32 gdi32 user32 shell32 advapi32 winmm)
  endif()
  burst_copy_mingw_runtime_dlls(burst-gui-smoke)
endif()
