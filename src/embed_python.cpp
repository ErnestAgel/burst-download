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

/* 从可执行文件路径提取所在目录 */
std::string ExeDirOf(const std::string& exe_path) {
  size_t slash = exe_path.find_last_of("/\\");
  return (slash != std::string::npos) ? exe_path.substr(0, slash) : "";
}

/* 定位运行时资源目录：exe 同目录 python_runtime/ → 环境变量 CURLBOLT_PYHOME → 编译期宏 */
std::string LocateRuntimeHome(const std::string& exe_path) {
  std::string home;
  std::string exe_dir = ExeDirOf(exe_path);
  if (!exe_dir.empty()) {
    std::string cand = exe_dir + "/python_runtime";
    if (access((cand + "/stdlib").c_str(), R_OK) == 0) home = cand;
  }
  if (home.empty()) {
    const char* env = getenv("CURLBOLT_PYHOME");
    if (env != nullptr && *env != '\0' &&
        access((std::string(env) + "/stdlib").c_str(), R_OK) == 0) {
      home = env;
    }
  }
  if (home.empty()) home = PYTHON_RUNTIME_FALLBACK;
  return home;
}

/* 临时 JSON 结果文件路径（进程内唯一） */
std::string ResultFile() {
  return g_python_home + "/.result_" + std::to_string(static_cast<long>(getpid())) + ".json";
}

/* 读取 yt_dlp/version.py 中的 __version__ 值（读不到返回空串） */
std::string ReadPackageVersion(const std::string& version_file) {
  std::ifstream in(version_file);
  if (!in.is_open()) return "";
  std::string line;
  while (std::getline(in, line)) {
    size_t p = line.find("__version__");
    if (p == std::string::npos) continue;
    size_t q1 = line.find('\'', p);
    if (q1 == std::string::npos) q1 = line.find('"', p);
    if (q1 != std::string::npos) {
      size_t q2 = line.find(line[q1], q1 + 1);
      if (q2 != std::string::npos) return line.substr(q1 + 1, q2 - q1 - 1);
    }
  }
  return "";
}

/* 生成安全的 Python 单引号字符串字面量（转义反斜杠与引号） */
std::string PyStr(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\\' || c == '\'') out += '\\';
    out += c;
  }
  out += "'";
  return out;
}

/* 极简 JSON 字符串字段提取（"key": "value"） */
std::string ExtractJsonStr(const std::string& content, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t p = content.find(pat);
  if (p == std::string::npos) return "";
  size_t q1 = content.find('"', p + pat.size());
  if (q1 == std::string::npos) return "";
  size_t q2 = content.find('"', q1 + 1);
  if (q2 == std::string::npos) return "";
  return content.substr(q1 + 1, q2 - q1 - 1);
}

}  // namespace

bool EmbedPythonInit(const std::string& python_home) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (g_initialized) return true;

  /* 定位运行时资源目录：显式传入 → exe 同目录 → 环境变量 → 编译期宏 */
  std::string home = python_home;
  if (!home.empty() && access((home + "/stdlib").c_str(), R_OK) != 0) {
    home.clear();  /* 传入路径无效，继续回退 */
  }
  if (home.empty()) home = LocateRuntimeHome("");
  if (home.empty()) return false;

  /* 校验 stdlib 存在 */
  if (access((home + "/stdlib").c_str(), R_OK) != 0) {
    fprintf(stderr, "[embed_python] 找不到 Python 运行时资源: %s\n", home.c_str());
    return false;
  }

  PyConfig config;
  PyConfig_InitPythonConfig(&config);
  PyConfig_SetBytesString(&config, &config.program_name, "burst");
  /* 显式告知运行时根，避免 Python 打印 "Could not find platform independent libraries" 噪音 */
  PyConfig_SetBytesString(&config, &config.home, home.c_str());
  config.module_search_paths_set = 1;
  PyWideStringList_Append(&config.module_search_paths,
                          Py_DecodeLocale((home + "/stdlib").c_str(), nullptr));
  PyWideStringList_Append(&config.module_search_paths,
                          Py_DecodeLocale(home.c_str(), nullptr));
  /* Windows 嵌入的 Python 扩展模块（.pyd）目录；Linux runtime 无此目录，加路径无害 */
  PyWideStringList_Append(&config.module_search_paths,
                          Py_DecodeLocale((home + "/lib-dynload").c_str(), nullptr));
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
    "        vids = sorted([f for f in fmts if f.get('vcodec') and f.get('vcodec') != 'none' and f.get('url')],\n"
    "                      key=lambda f: (f.get('height') or 0, f.get('tbr') or 0), reverse=True)\n"
    "        auds = sorted([f for f in fmts if f.get('acodec') and f.get('acodec') != 'none' and f.get('url')],\n"
    "                      key=lambda f: (f.get('abr') or 0, f.get('tbr') or 0), reverse=True)\n"
    "        if vids and auds:\n"
    "            urls = [vids[0]['url'], auds[0]['url']]  # [0] 视频轨, [1] 音频轨\n"
    "        else:\n"
    "            urls = [f.get('url') for f in fmts if f.get('url')]\n"
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

bool EmbedUpdateParser(const std::string& exe_path, std::string& msg) {
  /* 定位运行时资源目录（与 Init 同一套回退链） */
  std::string home = LocateRuntimeHome(exe_path);
  if (home.empty()) {
    msg = "找不到 Python 运行时资源目录（可执行文件同目录 python_runtime/ 缺失）";
    return false;
  }

  /* 当前解析器版本（无 version.py 时为空串，跳过版本比较直接更新） */
  std::string cur_ver = ReadPackageVersion(home + "/yt_dlp/version.py");

  if (!EmbedPythonInit(home)) {
    msg = "Python 运行时初始化失败，无法执行在线更新";
    return false;
  }

  const std::string result_file = ResultFile();
  /* Python 脚本：查 GitHub 最新版 → 下载 yt-dlp.tar.gz → 解压 → 原子替换 yt_dlp/ */
  std::string script;
  script.reserve(4096);
  script +=
    "import json, os, shutil, ssl, sys, tarfile, urllib.request\n"
    "import certifi\n"
    "OUT = " + PyStr(result_file) + "\n"
    "HOME = " + PyStr(home) + "\n"
    "CUR = " + PyStr(cur_ver) + "\n"
    "out = {'ok': False, 'msg': '', 'old': '', 'new': ''}\n"
    "class UpdateAbort(Exception):\n"
    "    pass\n"
    "def abort(msg):\n"
    "    out['msg'] = msg\n"
    "    raise UpdateAbort()\n"
    "try:\n"
    "    ctx = ssl.create_default_context(cafile=certifi.where())\n"
    "    api = 'https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest'\n"
    "    req = urllib.request.Request(api, headers={'User-Agent': 'burst/1.0', 'Accept': 'application/vnd.github+json'})\n"
    "    with urllib.request.urlopen(req, timeout=30, context=ctx) as r:\n"
    "        rel = json.load(r)\n"
    "    tag = rel.get('tag_name', '')\n"
    "    url = ''\n"
    "    for a in rel.get('assets') or []:\n"
    "        if a.get('name') == 'yt-dlp.tar.gz':\n"
    "            url = a.get('browser_download_url') or ''\n"
    "            break\n"
    "    if not url:\n"
    "        url = 'https://github.com/yt-dlp/yt-dlp/releases/download/%s/yt-dlp.tar.gz' % tag\n"
    "    if not tag or not url:\n"
    "        abort('获取最新版本信息失败')\n"
    "    out['new'] = tag\n"
    "    if CUR and CUR == tag:\n"
    "        out['ok'] = True\n"
    "        out['msg'] = 'already_latest'\n"
    "    else:\n"
    "        pid = os.getpid()\n"
    "        tgz = os.path.join(HOME, '.parser_update_%d.tar.gz' % pid)\n"
    "        exdir = os.path.join(HOME, '.parser_extract_%d' % pid)\n"
    "        try:\n"
    "            req2 = urllib.request.Request(url, headers={'User-Agent': 'burst/1.0'})\n"
    "            with urllib.request.urlopen(req2, timeout=120, context=ctx) as r2, open(tgz, 'wb') as f:\n"
    "                shutil.copyfileobj(r2, f, 65536)\n"
    "            if os.path.getsize(tgz) == 0:\n"
    "                abort('下载的更新包为空')\n"
    "            os.makedirs(exdir, exist_ok=True)\n"
    "            with tarfile.open(tgz, 'r:gz') as tf:\n"
    "                try:\n"
    "                    tf.extractall(exdir, filter='data')\n"
    "                except TypeError:\n"
    "                    tf.extractall(exdir)\n"
    "            pkg = os.path.join(exdir, 'yt_dlp')\n"
    "            if not os.path.isdir(pkg):\n"
    "                subs = [d for d in os.listdir(exdir) if os.path.isdir(os.path.join(exdir, d))]\n"
    "                if len(subs) == 1:\n"
    "                    pkg = os.path.join(exdir, subs[0], 'yt_dlp')\n"
    "            if not os.path.isfile(os.path.join(pkg, '__init__.py')):\n"
    "                abort('更新包内容不完整，已中止（现有版本保留）')\n"
    "            dst = os.path.join(HOME, 'yt_dlp')\n"
    "            bak = os.path.join(HOME, '.parser_bak_%d' % pid)\n"
    "            had_old = os.path.isdir(dst)\n"
    "            if had_old:\n"
    "                os.rename(dst, bak)\n"
    "            try:\n"
    "                os.rename(pkg, dst)\n"
    "            except Exception:\n"
    "                if had_old:\n"
    "                    os.rename(bak, dst)\n"
    "                raise\n"
    "            if had_old:\n"
    "                shutil.rmtree(bak, ignore_errors=True)\n"
    "            out['old'] = CUR\n"
    "            out['ok'] = True\n"
    "            out['msg'] = 'updated'\n"
    "        finally:\n"
    "            if os.path.isfile(tgz):\n"
    "                os.remove(tgz)\n"
    "            if os.path.isdir(exdir):\n"
    "                shutil.rmtree(exdir, ignore_errors=True)\n"
    "except UpdateAbort:\n"
    "    pass\n"
    "except Exception as e:\n"
    "    out['msg'] = '更新失败: %r' % (e,)\n"
    "json.dump(out, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False)\n";

  int rc = PyRun_SimpleString(script.c_str());
  if (rc != 0) {
    msg = "更新脚本执行失败";
    return false;
  }

  /* 读回 JSON */
  std::ifstream in(result_file);
  if (!in.is_open()) {
    msg = "更新结果读取失败";
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();
  remove(result_file.c_str());

  bool ok = content.find("\"ok\": true") != std::string::npos;
  std::string st = ExtractJsonStr(content, "msg");
  std::string old_ver = ExtractJsonStr(content, "old");
  std::string new_ver = ExtractJsonStr(content, "new");

  if (!ok) {
    msg = st.empty() ? "未知错误" : st;
    return false;
  }
  if (st == "already_latest") {
    msg = "视频解析组件已是最新版本 (" + (new_ver.empty() ? cur_ver : new_ver) +
          ")，无需更新";
    return true;
  }
  if (st == "updated") {
    msg = "视频解析组件已更新: " + (old_ver.empty() ? "旧版" : old_ver) + " → " +
          new_ver + "（立即生效，无需重新编译）";
    return true;
  }
  msg = st.empty() ? "未知结果" : st;
  return true;
}

void EmbedPythonShutdown() {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  if (g_initialized) {
    Py_Finalize();
    g_initialized = false;
  }
}
