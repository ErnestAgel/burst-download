/**
 * @file naming.cpp
 * @brief CLI naming helper implementations (P9 workflow module).
 */

#include "naming.h"

#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

bool FileExists(const std::string& strPath)
{
    return access(strPath.c_str(), F_OK) == 0;
}

bool VideoOutputExists(const std::string& strBasename)
{
    const char* kExts[] = {".mp4", ".m4a", ".mkv", ".webm"};
    for (const char* pszExt : kExts)
    {
        if (FileExists(strBasename + pszExt) ||
            FileExists(strBasename + "_full" + pszExt))
        {
            return true;
        }
    }
    return false;
}

std::string CurrentTimeStamp()
{
    char szBuf[32];
    const time_t tNow = time(nullptr);
    struct tm* ptmNow = localtime(&tNow);
    if (ptmNow != nullptr)
    {
        strftime(szBuf, sizeof(szBuf), "%Y%m%d_%H%M%S", ptmNow);
    }
    else
    {
        snprintf(szBuf, sizeof(szBuf), "%lld", (long long)tNow);
    }
    return std::string(szBuf);
}

std::string StampName(const std::string& strBase)
{
    const std::string strTs = CurrentTimeStamp();
    const size_t nDot = strBase.find_last_of('.');
    const size_t nSlash = strBase.find_last_of("/\\");
    if ((nDot != std::string::npos) &&
        ((nSlash == std::string::npos) || (nDot > nSlash)))
    {
        return strBase.substr(0, nDot) + "_" + strTs + strBase.substr(nDot);
    }
    return strBase + "_" + strTs;
}
