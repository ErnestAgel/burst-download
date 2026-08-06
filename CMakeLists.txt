cmake_minimum_required(VERSION 3.10)


project("curlbolt")

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
add_executable(curlbolt ${SRC})

# ---- 平台目录：third_party/<PLATFORM_DIR> ----
if(WIN32)
  set(PLATFORM_DIR windows-x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(PLATFORM_DIR linux-aarch64)
else()
  set(PLATFORM_DIR linux-x86_64)
endif()

# ==================== Release：静态链接（单文件，用于发布） ====================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  message(STATUS "Release 构建：静态链接 third_party/${PLATFORM_DIR}/libcurl.a")
  set(CURL_STATIC_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/static/libcurl.a)

  if(WIN32)
    # Windows: Schannel 原生 TLS，无需 openssl
    add_compile_definitions(CURL_STATICLIB)
    target_link_libraries(curlbolt ${CURL_STATIC_LIB} crypt32 ws2_32 bcrypt)
    set_target_properties(curlbolt PROPERTIES
      LINK_FLAGS "-static-libgcc -static-libstdc++ -static")
  else()
    # Linux: libcurl.a + 静态 openssl/zlib
    target_link_libraries(curlbolt ${CURL_STATIC_LIB})

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
      target_link_libraries(curlbolt ${THIRD_PARTY_STATIC_LIBS})
    else()
      # 回退：手动传参（兼容旧方式）
      set(OPENSSL_ROOT "" CACHE PATH "静态 openssl 安装目录（含 lib/libssl.a、lib/libcrypto.a）")
      if(OPENSSL_ROOT)
        target_link_libraries(curlbolt
          ${OPENSSL_ROOT}/lib/libssl.a ${OPENSSL_ROOT}/lib/libcrypto.a)
      endif()
      # 其他静态依赖（zlib/zstd 等），由构建脚本按需传入
      set(CURL_STATIC_DEPS "" CACHE STRING "额外静态依赖库列表（空格分隔）")
      if(CURL_STATIC_DEPS)
        target_link_libraries(curlbolt ${CURL_STATIC_DEPS})
      endif()
    endif()
    target_link_libraries(curlbolt dl pthread)
    # 全静态单文件（-static），目标机无需任何 libcurl/openssl 动态库
    set_target_properties(curlbolt PROPERTIES
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
    target_link_libraries(curlbolt ${CURL_TARGETS})
    # 运行时 libcurl-4.dll 需与可执行文件同目录（mingw import lib 记录的 DLL 名为 libcurl-4.dll）
    if(EXISTS ${CURL_WIN_DIR}/libcurl-4.dll)
      add_custom_command(TARGET curlbolt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CURL_WIN_DIR}/libcurl-4.dll $<TARGET_FILE_DIR:curlbolt>)
    endif()
    # MSVC 链接 libcurl 所需的系统库
    if(MSVC)
      target_link_libraries(curlbolt ws2_32 crypt32 wldap32 normaliz)
    endif()
  else()
    set(CURL_TARGETS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic/libcurl.so)
    target_link_libraries(curlbolt pthread ${CURL_TARGETS})
    # 运行时 rpath：确保可执行文件加载项目 third_party/<platform>/dynamic/ 下的 libcurl.so.4，而非系统库
    set_target_properties(curlbolt PROPERTIES
      BUILD_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic
      INSTALL_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${PLATFORM_DIR}/dynamic)
  endif()
endif()
