cmake_minimum_required(VERSION 3.10)


project("curl_download")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 项目自带头文件（跨平台）
set(CURL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CURL_INCLUDE_DIR})

file(GLOB SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_compile_options(-g)
add_executable(curl_download ${SRC})

# ---- 平台检测：Windows / Linux x86_64 / Linux aarch64 ----
if(WIN32)
  # Windows: 优先链接项目自带的 mingw 版 libcurl，缺失时回退系统 curl（vcpkg / 官方安装包）
  set(CURL_WIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/windows-x86_64)
  if(EXISTS ${CURL_WIN_DIR}/libcurl.dll.a)
    set(CURL_TARGETS ${CURL_WIN_DIR}/libcurl.dll.a)
  else()
    find_package(CURL REQUIRED)
    set(CURL_TARGETS CURL::libcurl)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  # Linux ARM64: 项目自带 ARM64 库
  set(CURL_TARGETS ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux-aarch64/libcurl.so)
  set(CURL_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux-aarch64)
else()
  # Linux x86_64: 项目自带 x86_64 库
  set(CURL_TARGETS ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux-x86_64/libcurl.so)
  set(CURL_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux-x86_64)
endif()

if(WIN32)
  target_link_libraries(curl_download ${CURL_TARGETS})
  # 运行时 libcurl-4.dll 需与可执行文件同目录（mingw import lib 记录的 DLL 名为 libcurl-4.dll）
  if(EXISTS ${CURL_WIN_DIR}/libcurl-4.dll)
    add_custom_command(TARGET curl_download POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CURL_WIN_DIR}/libcurl-4.dll $<TARGET_FILE_DIR:curl_download>)
  endif()
  # MSVC 链接 libcurl 所需的系统库
  if(MSVC)
    target_link_libraries(curl_download ws2_32 crypt32 wldap32 normaliz)
  endif()
else()
  target_link_libraries(curl_download pthread ${CURL_TARGETS})
  # 运行时 rpath：确保可执行文件加载项目 lib/ 下的 libcurl.so.4，而非系统库
  set_target_properties(curl_download PROPERTIES
    BUILD_RPATH ${CURL_RPATH}
    INSTALL_RPATH ${CURL_RPATH})
endif()
