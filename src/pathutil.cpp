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

/**
 * @brief Convert a hex digit to its value.
 * @param ch Character to convert.
 * @return 0..15 for valid hex digits, -1 otherwise.
 */
static s32 HexDigitValue(char ch)
{
    if ((ch >= '0') && (ch <= '9'))
    {
        return ch - '0';
    }
    if ((ch >= 'a') && (ch <= 'f'))
    {
        return ch - 'a' + 10;
    }
    if ((ch >= 'A') && (ch <= 'F'))
    {
        return ch - 'A' + 10;
    }
    return -1;
}

/**
 * @brief Decode percent-encoded octets (%XX) in place semantics.
 * @param strIn Encoded input.
 * @return Decoded string; invalid sequences are kept verbatim.
 */
static std::string PercentDecode(const std::string& strIn)
{
    std::string strOut;
    strOut.reserve(strIn.size());
    for (size_t nIndex = 0; nIndex < strIn.size(); ++nIndex)
    {
        if ((strIn[nIndex] == '%') && (nIndex + 2 < strIn.size()))
        {
            s32 nHigh = HexDigitValue(strIn[nIndex + 1]);
            s32 nLow = HexDigitValue(strIn[nIndex + 2]);
            if ((nHigh >= 0) && (nLow >= 0))
            {
                strOut += (char)((nHigh << 4) | nLow);
                nIndex += 2;
                continue;
            }
        }
        strOut += strIn[nIndex];
    }
    return strOut;
}

/**
 * @brief Check whether a name base is a Windows reserved device name.
 * @param strBase Name before the first dot, case-insensitive.
 * @return TRUE when reserved (CON/PRN/AUX/NUL/COM1-9/LPT1-9).
 */
static BOOL32 IsReservedDeviceName(const std::string& strBase)
{
    static const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string strUpper = strBase;
    for (size_t nIndex = 0; nIndex < strUpper.size(); ++nIndex)
    {
        if ((strUpper[nIndex] >= 'a') && (strUpper[nIndex] <= 'z'))
        {
            strUpper[nIndex] = (char)(strUpper[nIndex] - 32);
        }
    }
    for (const char* pszName : kReserved)
    {
        if (strUpper == pszName)
        {
            return TRUE;
        }
    }
    return FALSE;
}

std::string SanitizeFileName(const std::string& strName)
{
    std::string strOut = PercentDecode(strName);
    for (size_t nIndex = 0; nIndex < strOut.size(); ++nIndex)
    {
        const char ch = strOut[nIndex];
        const u8 byCh = (u8)ch;
        if ((byCh == '/') || (byCh == '\\') || (byCh < 0x20) ||
            (byCh == 0x7F) || (byCh == '<') || (byCh == '>') ||
            (byCh == ':') || (byCh == '"') || (byCh == '|') ||
            (byCh == '?') || (byCh == '*'))
        {
            strOut[nIndex] = '_';
        }
    }
    while ((strOut.empty() == false) &&
           ((strOut.back() == '.') || (strOut.back() == ' ')))
    {
        strOut.pop_back();
    }
    if (strOut.empty() || (strOut == ".") || (strOut == ".."))
    {
        return "";
    }
    const size_t nDot = strOut.find('.');
    if (IsReservedDeviceName(strOut.substr(0, nDot)))
    {
        strOut = "_" + strOut;
    }
    const size_t kMaxNameLen = 200;
    if (strOut.size() > kMaxNameLen)
    {
        strOut = strOut.substr(0, kMaxNameLen);
    }
    return strOut;
}
