/**
 * @file test_framework.cpp
 * @brief Test reporter implementation (see test_framework.h).
 */

#include "test_framework.h"

CTestReport::CTestReport()
    : m_strCurrentCase(""), m_dwTotal(0), m_dwFailed(0)
{
}

void CTestReport::BeginCase(const std::string& strName)
{
    m_strCurrentCase = strName;
    /* Case-level progress on stderr: when a test hangs, CI logs show the
     * exact case that never finished instead of an empty "Run tests" step. */
    std::fprintf(stderr, "[case] %s\n", strName.c_str());
    std::fflush(stderr);
}

void CTestReport::ExpectTrue(BOOL32 bCondition, const char* pszExpr,
                             const char* pszFile, u32 dwLine)
{
    ++m_dwTotal;
    if (bCondition == 0)
    {
        ++m_dwFailed;
        std::printf("[FAIL] %s (%s:%u) %s\n", m_strCurrentCase.c_str(),
                    pszFile, dwLine, pszExpr);
    }
}

void CTestReport::ExpectStrEq(const std::string& strExpected,
                              const std::string& strActual,
                              const char* pszExpr, const char* pszFile,
                              u32 dwLine)
{
    ++m_dwTotal;
    if (strExpected != strActual)
    {
        ++m_dwFailed;
        std::printf("[FAIL] %s (%s:%u) %s\n", m_strCurrentCase.c_str(),
                    pszFile, dwLine, pszExpr);
        std::printf("  expected: %s\n", strExpected.c_str());
        std::printf("  actual  : %s\n", strActual.c_str());
    }
}

s32 CTestReport::Finish()
{
    std::printf("test summary: %u passed, %u failed, %u total\n",
                m_dwTotal - m_dwFailed, m_dwFailed, m_dwTotal);
    if (m_dwFailed == 0)
    {
        return 0;
    }
    return 1;
}
