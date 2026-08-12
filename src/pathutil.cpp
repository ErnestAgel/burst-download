/**
 * @file pathutil.cpp
 * @brief Path/URL helper implementations.
 */

#include "pathutil.h"

std::string UrlBaseName(const std::string& strUrl)
{
    std::string strPath = strUrl;
    size_t nQuery = strPath.find_first_of("?#");
    if (nQuery != std::string::npos)
    {
        strPath = strPath.substr(0, nQuery);
    }
    while ((strPath.empty() == false) && (strPath.back() == '/'))
    {
        strPath.pop_back();
    }
    size_t nSlash = strPath.find_last_of("/\\");
    if (nSlash != std::string::npos)
    {
        return strPath.substr(nSlash + 1);
    }
    return strPath;
}
