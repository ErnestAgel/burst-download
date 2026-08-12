#ifndef BURST_TEST_FRAMEWORK_H
#define BURST_TEST_FRAMEWORK_H

/**
 * @file test_framework.h
 * @brief Minimal self-contained unit-test reporter for Burst Download.
 *
 * Design notes:
 * - Zero external dependencies (no doctest/Catch2 download needed).
 * - No function pointers: each test module exposes a RunXxxTests(CTestReport&)
 *   function that calls its test cases in order.
 * - Assertions are the project's custom macros; built-in assert is not used.
 */

#include <cstdio>
#include <string>

#include "burst_types.h"

/** @brief Records per-case pass/fail counts and prints failures. */
class CTestReport
{
public:
    CTestReport();

    /**
     * @brief Start a named test case (name appears in failure output).
     * @param strName Case name.
     */
    void BeginCase(const std::string& strName);

    /**
     * @brief Check a boolean condition.
     * @param bCondition Expected TRUE.
     * @param pszExpr Source expression text.
     * @param pszFile Source file name.
     * @param dwLine Source line number.
     */
    void ExpectTrue(BOOL32 bCondition, const char* pszExpr,
                    const char* pszFile, u32 dwLine);

    /**
     * @brief Check string equality.
     * @param strExpected Expected value.
     * @param strActual Actual value.
     * @param pszExpr Expression text.
     * @param pszFile Source file name.
     * @param dwLine Source line number.
     */
    void ExpectStrEq(const std::string& strExpected,
                     const std::string& strActual,
                     const char* pszExpr, const char* pszFile, u32 dwLine);

    /**
     * @brief Print the summary and return the process exit code.
     * @return 0 when all cases passed, 1 otherwise.
     */
    s32 Finish();

private:
    std::string m_strCurrentCase;
    u32 m_dwTotal;
    u32 m_dwFailed;
};

/** @brief Assert a boolean expression (registers pass/fail). */
#define BURST_EXPECT_TRUE(cReport, expr)                                      \
    (cReport).ExpectTrue(((expr) != 0) ? 1 : 0, #expr, __FILE__, __LINE__)

/** @brief Assert string equality (registers pass/fail). */
#define BURST_EXPECT_STR_EQ(cReport, strExpected, strActual)                   \
    (cReport).ExpectStrEq((strExpected), (strActual),                          \
                          #strActual " == " #strExpected, __FILE__, __LINE__)

#endif  // BURST_TEST_FRAMEWORK_H
