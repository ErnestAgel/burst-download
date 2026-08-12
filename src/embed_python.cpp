/**
 * @file embed_python.cpp
 * @brief In-process embedded CPython + yt_dlp implementation.
 *
 * Implementation notes:
 *   - PyConfig explicitly sets the module search paths (stdlib directory +
 *     yt_dlp directory).  Zip paths are not usable while the encodings codec
 *     loads during early initialization, so extracted directories are used.
 *   - Parse results are written by Python to a temporary JSON file that C++
 *     reads back (avoids stdout capture).
 *   - Cookies: cookies_from_browser uses yt-dlp cookiesfrombrowser; a manual
 *     cookie goes through the http_headers Cookie header.
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include "embed_python.h"
#include "embedded_runtime.h"

#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef PYTHON_RUNTIME_FALLBACK
#define PYTHON_RUNTIME_FALLBACK ""
#endif

namespace {

std::mutex g_init_mutex;
bool g_initialized = false;
std::string g_python_home;

/* Auto-update throttle: at most one GitHub check per 24h per runtime dir. */
constexpr long kParserAutoUpdateIntervalSec = 24 * 3600;

/* Retry window after a failed update attempt (do not hammer GitHub). */
constexpr long kParserRetryAfterFailSec = 3600;

/** @brief Directory part of an executable path. */
std::string ExeDirOf(const std::string& strExePath)
{
    const size_t nSlash = strExePath.find_last_of("/\\");
    return (nSlash != std::string::npos) ? strExePath.substr(0, nSlash) : "";
}

/** @brief Runtime root is usable when stdlib/ or python311.zip exists. */
bool RuntimeHomeUsable(const std::string& strHome)
{
    return access((strHome + "/stdlib").c_str(), R_OK) == 0 ||
           access((strHome + "/python311.zip").c_str(), R_OK) == 0;
}

/** @brief Locate the runtime assets directory: exe assets/ -> temp cache ->
 *         CURLBOLT_PYHOME -> compile-time macro (dev source tree). */
std::string LocateRuntimeHome(const std::string& strExePath)
{
    std::string strHome;
    const std::string strExe =
        strExePath.empty() ? EmbedGetExePath() : strExePath;
    const std::string strExeDir = ExeDirOf(strExe);
    if (!strExeDir.empty())
    {
        std::string strCand = strExeDir + "/assets";
        if (!RuntimeHomeUsable(strCand))
        {
            /* Legacy directory name python_runtime/ (still usable when the
             * old extracted layout is replaced in place next to the exe). */
            strCand = strExeDir + "/python_runtime";
        }
        if (RuntimeHomeUsable(strCand))
        {
            strHome = strCand;
        }
    }
    if (strHome.empty())
    {
        std::string strCached;
        if (ExtractEmbeddedRuntime(strCached) && !strCached.empty() &&
            RuntimeHomeUsable(strCached))
        {
            strHome = strCached;
        }
    }
    if (strHome.empty())
    {
        const char* pszEnv = getenv("CURLBOLT_PYHOME");
        if (pszEnv != nullptr && *pszEnv != '\0' &&
            RuntimeHomeUsable(pszEnv))
        {
            strHome = pszEnv;
        }
    }
    if (strHome.empty())
    {
        strHome = PYTHON_RUNTIME_FALLBACK;
    }
    return strHome;
}

/** @brief Temporary JSON result file path (unique per process). */
std::string ResultFile()
{
    return g_python_home + "/.result_" +
           std::to_string(static_cast<long>(getpid())) + ".json";
}

/** @brief Read the parser version: version_ytdlp.txt first, then the legacy
 *         yt_dlp/version.py layout. */
std::string ReadVersionMarker(const std::string& strHome)
{
    std::ifstream inFile(strHome + "/version_ytdlp.txt");
    std::string strVer;
    std::getline(inFile, strVer);
    if (!inFile)
    {
        std::ifstream vf(strHome + "/yt_dlp/version.py");
        std::string strLine;
        while (std::getline(vf, strLine))
        {
            const size_t nPos = strLine.find("__version__");
            if (nPos == std::string::npos)
            {
                continue;
            }
            size_t nQ1 = strLine.find('\'', nPos);
            if (nQ1 == std::string::npos)
            {
                nQ1 = strLine.find('"', nPos);
            }
            if (nQ1 != std::string::npos)
            {
                const size_t nQ2 = strLine.find(strLine[nQ1], nQ1 + 1);
                if (nQ2 != std::string::npos)
                {
                    strVer = strLine.substr(nQ1 + 1, nQ2 - nQ1 - 1);
                }
            }
            break;
        }
    }
    while (!strVer.empty() &&
           (strVer.back() == '\r' || strVer.back() == '\n' ||
            strVer.back() == ' ' || strVer.back() == '\t'))
    {
        strVer.pop_back();
    }
    return strVer;
}

/* Base64 encode (alphabet only A-Za-z0-9+/=, no quotes/newlines/backslashes):
 * all user-controlled values are passed into the embedded Python scripts as
 * base64 literals and decoded by the script, preventing control characters
 * in URLs/cookies from closing string literals (script injection, S1). */
std::string Base64Encode(const std::string& strIn)
{
    static const char kTbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string strOut;
    strOut.reserve((strIn.size() + 2) / 3 * 4);
    size_t nIndex = 0;
    while (nIndex + 2 < strIn.size())
    {
        uint32_t dwValue = ((uint8_t)strIn[nIndex] << 16) |
                           ((uint8_t)strIn[nIndex + 1] << 8) |
                           (uint8_t)strIn[nIndex + 2];
        strOut += kTbl[(dwValue >> 18) & 63];
        strOut += kTbl[(dwValue >> 12) & 63];
        strOut += kTbl[(dwValue >> 6) & 63];
        strOut += kTbl[dwValue & 63];
        nIndex += 3;
    }
    const size_t nRem = strIn.size() - nIndex;
    if (nRem == 1)
    {
        const uint32_t dwValue = (uint8_t)strIn[nIndex] << 16;
        strOut += kTbl[(dwValue >> 18) & 63];
        strOut += kTbl[(dwValue >> 12) & 63];
        strOut += "==";
    }
    else if (nRem == 2)
    {
        const uint32_t dwValue =
            ((uint8_t)strIn[nIndex] << 16) | ((uint8_t)strIn[nIndex + 1] << 8);
        strOut += kTbl[(dwValue >> 18) & 63];
        strOut += kTbl[(dwValue >> 12) & 63];
        strOut += kTbl[(dwValue >> 6) & 63];
        strOut += '=';
    }
    return strOut;
}

/** @brief Python string literal: '<base64>' (safe inside single quotes). */
std::string B64Lit(const std::string& strIn)
{
    return "'" + Base64Encode(strIn) + "'";
}

/** @brief Whitelist of browsers supported by yt-dlp cookiesfrombrowser. */
bool ValidBrowserName(const std::string& strBrowser)
{
    static const char* kNames[] = {
        "brave", "chrome", "chromium", "edge", "firefox",
        "opera", "safari", "vivaldi", "whale"};
    for (const char* pszName : kNames)
    {
        if (strBrowser == pszName)
        {
            return true;
        }
    }
    return false;
}

/** @brief Minimal JSON string field extraction ("key": "value"). */
std::string ExtractJsonStr(const std::string& strContent,
                           const std::string& strKey)
{
    const std::string strPattern = "\"" + strKey + "\"";
    const size_t nPos = strContent.find(strPattern);
    if (nPos == std::string::npos)
    {
        return "";
    }
    const size_t nQ1 = strContent.find('"', nPos + strPattern.size());
    if (nQ1 == std::string::npos)
    {
        return "";
    }
    const size_t nQ2 = strContent.find('"', nQ1 + 1);
    if (nQ2 == std::string::npos)
    {
        return "";
    }
    return strContent.substr(nQ1 + 1, nQ2 - nQ1 - 1);
}

/** @brief Run the yt_dlp online check/update for the given runtime home
 *         (shared by the manual --update-parser and the auto update). */
bool UpdateParserAt(const std::string& strHome, std::string& strMsg)
{
    /* Current parser version; empty when no marker file exists, in which
     * case the version comparison is skipped and an update is attempted. */
    const std::string strCurVer = ReadVersionMarker(strHome);

    if (!EmbedPythonInit(strHome))
    {
        strMsg = "Python runtime init failed, cannot update online";
        return false;
    }

    const std::string strResultFile = ResultFile();
    /* Python script: query the latest GitHub release -> download
     * yt-dlp.tar.gz -> safe extract -> atomic replace of yt_dlp/. */
    std::string strScript;
    strScript.reserve(4096);
    strScript +=
        "import json, os, re, shutil, ssl, sys, tarfile, urllib.request, "
        "base64\n"
        "import certifi\n"
        "def _b(s): return base64.b64decode(s.encode('ascii')).decode("
        "'utf-8', 'replace')\n"
        "OUT = _b(" + B64Lit(strResultFile) + ")\n"
        "HOME = _b(" + B64Lit(strHome) + ")\n"
        "CUR = _b(" + B64Lit(strCurVer) + ")\n"
        "out = {'ok': False, 'msg': '', 'old': '', 'new': ''}\n"
        "class UpdateAbort(Exception):\n"
        "    pass\n"
        "def abort(msg):\n"
        "    out['msg'] = msg\n"
        "    raise UpdateAbort()\n"
        "def _safe_extract(tf, exdir):\n"
        "    members = tf.getmembers()\n"
        "    for m in members:\n"
        "        name = m.name.replace('\\\\', '/')\n"
        "        if (name.startswith('/') or os.path.isabs(name) or\n"
        "                ('..' in name.split('/')) or\n"
        "                re.match(r'^[A-Za-z]:', name)):\n"
        "            abort('unsafe archive entry: ' + m.name)\n"
        "        if m.issym() or m.islnk():\n"
        "            target = (m.linkname or '').replace('\\\\', '/')\n"
        "            if (target.startswith('/') or os.path.isabs(target) or\n"
        "                    ('..' in target.split('/')) or\n"
        "                    re.match(r'^[A-Za-z]:', target)):\n"
        "                abort('unsafe archive link: ' + m.name)\n"
        "    tf.extractall(exdir, members=members)\n"
        "try:\n"
        "    ctx = ssl.create_default_context(cafile=certifi.where())\n"
        "    api = 'https://api.github.com/repos/yt-dlp/yt-dlp/releases/"
        "latest'\n"
        "    req = urllib.request.Request(api, headers={'User-Agent': "
        "'burst/1.0', 'Accept': 'application/vnd.github+json'})\n"
        "    with urllib.request.urlopen(req, timeout=10, context=ctx) as r:\n"
        "        rel = json.load(r)\n"
        "    tag = rel.get('tag_name', '')\n"
        "    url = ''\n"
        "    for a in rel.get('assets') or []:\n"
        "        if a.get('name') == 'yt-dlp.tar.gz':\n"
        "            url = a.get('browser_download_url') or ''\n"
        "            break\n"
        "    if not url:\n"
        "        url = 'https://github.com/yt-dlp/yt-dlp/releases/download/%s/"
        "yt-dlp.tar.gz' % tag\n"
        "    if not tag or not url:\n"
        "        abort('failed to fetch the latest release info')\n"
        "    out['new'] = tag\n"
        "    if CUR and CUR == tag:\n"
        "        out['ok'] = True\n"
        "        out['msg'] = 'already_latest'\n"
        "    else:\n"
        "        pid = os.getpid()\n"
        "        tgz = os.path.join(HOME, '.parser_update_%d.tar.gz' % pid)\n"
        "        exdir = os.path.join(HOME, '.parser_extract_%d' % pid)\n"
        "        try:\n"
        "            req2 = urllib.request.Request(url, headers={'User-Agent': "
        "'burst/1.0'})\n"
        "            with urllib.request.urlopen(req2, timeout=120, "
        "context=ctx) as r2, open(tgz, 'wb') as f:\n"
        "                shutil.copyfileobj(r2, f, 65536)\n"
        "            if os.path.getsize(tgz) == 0:\n"
        "                abort('downloaded update package is empty')\n"
        "            os.makedirs(exdir, exist_ok=True)\n"
        "            with tarfile.open(tgz, 'r:gz') as tf:\n"
        "                _safe_extract(tf, exdir)\n"
        "            pkg = os.path.join(exdir, 'yt_dlp')\n"
        "            if not os.path.isdir(pkg):\n"
        "                subs = [d for d in os.listdir(exdir) if "
        "os.path.isdir(os.path.join(exdir, d))]\n"
        "                if len(subs) == 1:\n"
        "                    pkg = os.path.join(exdir, subs[0], 'yt_dlp')\n"
        "            if not os.path.isfile(os.path.join(pkg, '__init__.py')):\n"
        "                abort('update package is incomplete, aborted "
        "(current version kept)')\n"
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
        "            import compileall, pathlib, py_compile\n"
        "            compileall.compile_dir(dst, quiet=1, legacy=True, "
        "force=True)\n"
        "            for f in list(pathlib.Path(dst).rglob('*.py')):\n"
        "                if f.name == 'version.py':\n"
        "                    continue\n"
        "                f.unlink()\n"
        "            for d in list(pathlib.Path(dst).rglob('__pycache__')):\n"
        "                shutil.rmtree(d, ignore_errors=True)\n"
        "            with open(os.path.join(HOME, 'version_ytdlp.txt'), 'w') "
        "as vf:\n"
        "                vf.write(tag + chr(10))\n"
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
        "    out['msg'] = 'update failed: %r' % (e,)\n"
        "json.dump(out, open(OUT, 'w', encoding='utf-8'), "
        "ensure_ascii=False)\n";

    const int nRc = PyRun_SimpleString(strScript.c_str());
    if (nRc != 0)
    {
        strMsg = "update script execution failed";
        return false;
    }

    /* Read back the JSON result. */
    std::ifstream inFile(strResultFile);
    if (!inFile.is_open())
    {
        strMsg = "failed to read the update result";
        return false;
    }
    std::string strContent((std::istreambuf_iterator<char>(inFile)),
                           std::istreambuf_iterator<char>());
    inFile.close();
    remove(strResultFile.c_str());

    const bool bOk =
        strContent.find("\"ok\": true") != std::string::npos;
    const std::string strState = ExtractJsonStr(strContent, "msg");
    const std::string strOldVer = ExtractJsonStr(strContent, "old");
    const std::string strNewVer = ExtractJsonStr(strContent, "new");

    if (!bOk)
    {
        strMsg = strState.empty() ? "unknown error" : strState;
        return false;
    }
    if (strState == "already_latest")
    {
        strMsg = "video parser is already up to date (" +
                 (strNewVer.empty() ? strCurVer : strNewVer) +
                 "), no update needed";
        return true;
    }
    if (strState == "updated")
    {
        strMsg = "video parser updated: " +
                 (strOldVer.empty() ? "old version" : strOldVer) + " -> " +
                 strNewVer + " (active immediately, no rebuild needed)";
        return true;
    }
    strMsg = strState.empty() ? "unknown result" : strState;
    return true;
}

}  // namespace

bool EmbedPythonInit(const std::string& strPythonHome)
{
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized)
    {
        return true;
    }

    /* Locate the runtime assets directory: explicit path -> exe assets/ ->
     * environment variable -> compile-time macro. */
    std::string strHome = strPythonHome;
    if (!strHome.empty() && !RuntimeHomeUsable(strHome))
    {
        strHome.clear();  /* invalid path, keep falling back */
    }
    if (strHome.empty())
    {
        strHome = LocateRuntimeHome("");
    }
    if (strHome.empty())
    {
        fprintf(stderr,
                "[embed_python] Python runtime assets not found: checked "
                "assets/ next to the exe, python_runtime/, the embedded "
                "runtime cache and CURLBOLT_PYHOME.\n"
                "[embed_python] Put assets/ (with stdlib/ and yt_dlp/) next "
                "to the executable and run again.\n");
        return false;
    }

    if (!RuntimeHomeUsable(strHome))
    {
        fprintf(stderr,
                "[embed_python] Python runtime assets not usable: %s\n",
                strHome.c_str());
        return false;
    }

    PyConfig tConfig;
    PyConfig_InitPythonConfig(&tConfig);
    PyConfig_SetBytesString(&tConfig, &tConfig.program_name, "burst");
    /* Explicit runtime root avoids Python printing "Could not find platform
     * independent libraries" noise. */
    PyConfig_SetBytesString(&tConfig, &tConfig.home, strHome.c_str());
    tConfig.module_search_paths_set = 1;
    /* stdlib: prefer the extracted directory (dev layout); the release
     * layout ships python311.zip (also auto-loaded early by CPython). */
    std::string strStdlib = strHome + "/stdlib";
    if (access(strStdlib.c_str(), R_OK) != 0)
    {
        strStdlib = strHome + "/python311.zip";
    }
    /* Real directories come first: zipimport is not available during early
     * initialization, so required modules (encodings etc.) load from them. */
    PyWideStringList_Append(&tConfig.module_search_paths,
                            Py_DecodeLocale(strHome.c_str(), nullptr));
    PyWideStringList_Append(&tConfig.module_search_paths,
                            Py_DecodeLocale(strStdlib.c_str(), nullptr));
    /* Windows .pyd extension directory; harmless on Linux (no such dir). */
    PyWideStringList_Append(&tConfig.module_search_paths,
                            Py_DecodeLocale(
                                (strHome + "/lib-dynload").c_str(), nullptr));
    const PyStatus stStatus = Py_InitializeFromConfig(&tConfig);
    if (PyStatus_Exception(stStatus))
    {
        fprintf(stderr, "[embed_python] Py_Initialize failed: %s\n",
                stStatus.err_msg ? stStatus.err_msg : "?");
        PyConfig_Clear(&tConfig);
        return false;
    }
    PyConfig_Clear(&tConfig);
    g_python_home = strHome;
    g_initialized = true;
    return true;
}

bool EmbedParseVideoUrls(const std::string& strUrl,
                         std::vector<std::string>& vecUrls,
                         const std::string& strCookiesFromBrowser,
                         const std::string& strCookie, std::string& strErr)
{
    vecUrls.clear();
    if (!g_initialized)
    {
        strErr = "Python runtime is not initialized";
        return false;
    }
    /* Input guards: length and browser-name whitelist (combined with base64
     * passing to rule out script injection). */
    if (strUrl.empty() || strUrl.size() > 8192 ||
        strUrl.find('\n') != std::string::npos ||
        strUrl.find('\r') != std::string::npos)
    {
        strErr = "invalid URL (empty, too long or contains control chars)";
        return false;
    }
    if (strCookie.size() > 4096 ||
        strCookie.find('\n') != std::string::npos ||
        strCookie.find('\r') != std::string::npos)
    {
        strErr = "invalid cookie (too long or contains control chars)";
        return false;
    }
    if (!strCookiesFromBrowser.empty() &&
        (!ValidBrowserName(strCookiesFromBrowser) ||
         strCookiesFromBrowser.size() > 64))
    {
        strErr = "unsupported browser name (supported: brave/chrome/"
                 "chromium/edge/firefox/opera/safari/vivaldi/whale)";
        return false;
    }

    const std::string strResultFile = ResultFile();
    /* Python script: run yt_dlp and write the JSON result to a temp file. */
    char szScript[16384];
    snprintf(szScript, sizeof(szScript),
        "import yt_dlp, json, base64\n"
        "def _b(s): return base64.b64decode(s.encode('ascii')).decode("
        "'utf-8', 'replace')\n"
        "try:\n"
        "    opts = {'quiet': True, 'skip_download': True, 'no_warnings': "
        "True}\n"
        "    _browser = _b(%s)\n"
        "    _cookie = _b(%s)\n"
        "    _url = _b(%s)\n"
        "    if _browser:\n"
        "        opts['cookiesfrombrowser'] = (_browser,)\n"
        "    if _cookie:\n"
        "        opts['http_headers'] = {'Cookie': _cookie}\n"
        "    with yt_dlp.YoutubeDL(opts) as ydl:\n"
        "        info = ydl.extract_info(_url, download=False)\n"
        "        fmts = info.get('formats') or []\n"
        "        vids = sorted([f for f in fmts if f.get('vcodec') and "
        "f.get('vcodec') != 'none' and f.get('url')],\n"
        "                      key=lambda f: (f.get('height') or 0, "
        "f.get('tbr') or 0), reverse=True)\n"
        "        auds = sorted([f for f in fmts if f.get('acodec') and "
        "f.get('acodec') != 'none' and f.get('url')],\n"
        "                      key=lambda f: (f.get('abr') or 0, "
        "f.get('tbr') or 0), reverse=True)\n"
        "        if vids and auds:\n"
        "            urls = [vids[0]['url'], auds[0]['url']]  # [0] video "
        "track, [1] audio track\n"
        "        else:\n"
        "            urls = [f.get('url') for f in fmts if f.get('url')]\n"
        "        if not urls:\n"
        "            if not fmts:\n"
        "                raise Exception('page parsed but no media formats "
        "returned: the video may require login cookies or be "
        "region-restricted')\n"
        "            raise Exception('parsed ' + str(len(fmts)) + ' formats "
        "but none has a usable direct URL: login cookies may be required')\n"
        "        json.dump({'title': info.get('title'), 'urls': urls},\n"
        "                  open(_b(%s), 'w', encoding='utf-8'), "
        "ensure_ascii=False)\n"
        "except Exception as e:\n"
        "    json.dump({'error': (str(e) or repr(e))},\n"
        "              open(_b(%s), 'w', encoding='utf-8'), "
        "ensure_ascii=False)\n",
        B64Lit(strCookiesFromBrowser).c_str(), B64Lit(strCookie).c_str(),
        B64Lit(strUrl).c_str(), B64Lit(strResultFile).c_str(),
        B64Lit(strResultFile).c_str());

    const int nRc = PyRun_SimpleString(szScript);
    if (nRc != 0)
    {
        strErr = "Python script execution failed";
        return false;
    }

    /* Read back the JSON result. */
    std::ifstream inFile(strResultFile);
    if (!inFile.is_open())
    {
        strErr = "failed to read the parse result";
        return false;
    }
    std::string strContent((std::istreambuf_iterator<char>(inFile)),
                           std::istreambuf_iterator<char>());
    inFile.close();
    remove(strResultFile.c_str());

    /* Minimal JSON parsing: extract the "urls" array strings. */
    const size_t nErrPos = strContent.find("\"error\"");
    if (nErrPos != std::string::npos)
    {
        strErr = strContent.substr(nErrPos + 8,
                                   strContent.size() - nErrPos - 10);
        return false;
    }
    const size_t nArr = strContent.find("\"urls\"");
    if (nArr == std::string::npos)
    {
        strErr = "no urls field in the result";
        return false;
    }
    size_t nIndex = strContent.find('[', nArr);
    while ((nIndex != std::string::npos) && (nIndex < strContent.size()))
    {
        const size_t nQ1 = strContent.find('"', nIndex);
        if (nQ1 == std::string::npos)
        {
            break;
        }
        const size_t nQ2 = strContent.find('"', nQ1 + 1);
        if (nQ2 == std::string::npos)
        {
            break;
        }
        vecUrls.push_back(strContent.substr(nQ1 + 1, nQ2 - nQ1 - 1));
        nIndex = strContent.find('"', nQ2 + 1);
    }
    return !vecUrls.empty();
}

bool EmbedUpdateParser(const std::string& strExePath, std::string& strMsg)
{
    /* Locate the runtime assets directory (same fallback chain as Init). */
    const std::string strHome = LocateRuntimeHome(strExePath);
    if (strHome.empty())
    {
        strMsg = "Python runtime assets directory not found (assets/ missing "
                 "next to the executable)";
        return false;
    }
    return UpdateParserAt(strHome, strMsg);
}

bool EmbedAutoUpdateParser(std::string& strMsg)
{
    /* Prefer the already initialized runtime home; probe otherwise. */
    std::string strHome = g_python_home;
    if (strHome.empty())
    {
        strHome = LocateRuntimeHome("");
    }
    if (strHome.empty())
    {
        strMsg = "Python runtime assets directory not found";
        return false;
    }

    /* Throttle: at most one check per 24h after a successful check; after a
     * failed attempt allow a retry after 1h (issue O8). */
    const std::string strCheckStamp = strHome + "/.parser_last_check";
    const std::string strFailStamp = strHome + "/.parser_last_fail";
    struct stat stCheck;
    struct stat stFail;
    const time_t tNow = time(nullptr);
    if ((stat(strCheckStamp.c_str(), &stCheck) == 0) &&
        (tNow >= stCheck.st_mtime) &&
        ((tNow - stCheck.st_mtime) < kParserAutoUpdateIntervalSec))
    {
        strMsg.clear();  /* checked recently: skip silently */
        return true;
    }
    if ((stat(strFailStamp.c_str(), &stFail) == 0) &&
        (tNow >= stFail.st_mtime) &&
        ((tNow - stFail.st_mtime) < kParserRetryAfterFailSec))
    {
        strMsg.clear();  /* failed recently: skip to avoid hammering GitHub */
        return true;
    }
    const bool bOk = UpdateParserAt(strHome, strMsg);
    if (bOk)
    {
        { std::ofstream f(strCheckStamp.c_str(), std::ios::app); }
        std::remove(strFailStamp.c_str());
    }
    else
    {
        { std::ofstream f(strFailStamp.c_str(), std::ios::app); }
    }
    return bOk;
}

void EmbedPythonShutdown()
{
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized)
    {
        Py_Finalize();
        g_initialized = false;
    }
}
