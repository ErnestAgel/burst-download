/**
 * @file embed_python.cpp
 * @brief 进程内嵌入 CPython + yt_dlp 的实现
 *
 * 实现要点：
 *   - PyConfig 显式指定模块搜索路径（stdlib 目录 + yt_dlp 目录）
 *     （初始化早期加载 encodings codec 时 zip 路径不可用，故用解压目录）
 *   - 解析结果通过 Python 写入临时 JSON 文件，C++ 读回（避免 stdout 捕获）
 *   - Cookie：cookies_from_browser 走 yt-dlp cookiesfrombrowser；
 *             手动 cookie 走 http_headers 的 Cookie 头
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include "embed_python.h"

#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifndef PYTHON_RUNTIME_FALLBACK
#define PYTHON_RUNTIME_FALLBACK ""
#endif

namespace {

std::mutex g_init_mutex;
bool g_initialized = false;
std::string g_python_home;

/* 从可执行文件路径推导同目录 python_runtime/（argv[0] 由调用方注入） */
std::string g_exe_dir;

/* 临时 JSON 结果文件路径（进程内唯一） */
std::string ResultFile() {
  return g_python_home + "/.result_" + std::to_string(static_cast<long>(getpid())) + ".json";
}

}  // namespace

bool EmbedPythonInit(const std::string& python_home) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (g_initialized) return true;

  /* 定位运行时资源目录：exe 同目录 → 环境变量 → 编译期宏 */
  std::string home = python_home;
  if (home.empty() && !g_exe_dir.empty()) {
    home = g_exe_dir + "/python_runtime";
  }
  if (!home.empty() && access((home + "/stdlib").c_str(), R_OK) != 0) {
    home.clear();  /* 传入路径无效，继续回退 */
  }
  if (home.empty()) {
    const char* env = getenv("CURLBOLT_PYHOME");
    if (env != nullptr && *env != '\0' &&
        access((std::string(env) + "/stdlib").c_str(), R_OK) == 0) {
      home = env;
    }
  }
  if (home.empty()) home = PYTHON_RUNTIME_FALLBACK;
  if (home.empty()) return false;

  /* 校验 stdlib 存在 */
  if (access((home + "/stdlib").c_str(), R_OK) != 0) {
    fprintf(stderr, "[embed_python] 找不到 Python 运行时资源: %s\n", home.c_str());
    return false;
  }

  PyConfig config;
  PyConfig_InitPythonConfig(&config);
  PyConfig_SetBytesString(&config, &config.program_name, "curlbolt");
  config.module_search_paths_set = 1;
  PyWideStringList_Append(&config.module_search_paths,
                          Py_DecodeLocale((home + "/stdlib").c_str(), nullptr));
  PyWideStringList_Append(&config.module_search_paths,
                          Py_DecodeLocale(home.c_str(), nullptr));
  PyStatus status = Py_InitializeFromConfig(&config);
  if (PyStatus_Exception(status)) {
    fprintf(stderr, "[embed_python] Py_Initialize 失败: %s\n",
            status.err_msg ? status.err_msg : "?");
    PyConfig_Clear(&config);
    return false;
  }
  PyConfig_Clear(&config);
  g_python_home = home;
  g_initialized = true;
  return true;
}

bool EmbedParseVideoUrls(const std::string& url,
                         std::vector<std::string>& urls,
                         const std::string& cookies_from_browser,
                         const std::string& cookie,
                         std::string& err) {
  urls.clear();
  if (!g_initialized) {
    err = "Python 运行时未初始化";
    return false;
  }

  const std::string result_file = ResultFile();
  /* Python 脚本：调 yt_dlp 解析，结果 JSON 写入临时文件 */
  char script[16384];
  snprintf(script, sizeof(script),
    "import yt_dlp, json\n"
    "try:\n"
    "    opts = {'quiet': True, 'skip_download': True, 'no_warnings': True}\n"
    "    if '%s':\n"
    "        opts['cookiesfrombrowser'] = ('%s',)\n"
    "    if '%s':\n"
    "        opts['http_headers'] = {'Cookie': '%s'}\n"
    "    with yt_dlp.YoutubeDL(opts) as ydl:\n"
    "        info = ydl.extract_info('%s', download=False)\n"
    "        fmts = info.get('formats') or []\n"
    "        urls = [f.get('url') for f in fmts if f.get('url')]\n"
    "        json.dump({'title': info.get('title'), 'urls': urls},\n"
    "                  open('%s', 'w'))\n"
    "except Exception as e:\n"
    "    json.dump({'error': repr(e)}, open('%s', 'w'))\n",
    cookies_from_browser.c_str(), cookies_from_browser.c_str(),
    cookie.c_str(), cookie.c_str(),
    url.c_str(), result_file.c_str(), result_file.c_str());

  int rc = PyRun_SimpleString(script);
  if (rc != 0) {
    err = "Python 脚本执行失败";
    return false;
  }

  /* 读回 JSON */
  std::ifstream in(result_file);
  if (!in.is_open()) {
    err = "解析结果读取失败";
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();
  remove(result_file.c_str());

  /* 极简 JSON 解析：只提取 "urls" 数组中的字符串 */
  size_t err_pos = content.find("\"error\"");
  if (err_pos != std::string::npos) {
    err = content.substr(err_pos + 8, content.size() - err_pos - 10);
    return false;
  }
  size_t arr = content.find("\"urls\"");
  if (arr == std::string::npos) {
    err = "结果中无 urls 字段";
    return false;
  }
  size_t i = content.find('[', arr);
  while (i != std::string::npos && i < content.size()) {
    size_t q1 = content.find('"', i);
    if (q1 == std::string::npos) break;
    size_t q2 = content.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    urls.push_back(content.substr(q1 + 1, q2 - q1 - 1));
    i = content.find('"', q2 + 1);
  }
  return !urls.empty();
}

void EmbedPythonShutdown() {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (g_initialized) {
    Py_Finalize();
    g_initialized = false;
  }
}
