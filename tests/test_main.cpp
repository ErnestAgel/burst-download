/**
 * @file test_main.cpp
 * @brief Unit-test entry point: runs every test module and reports.
 */

#include "test_framework.h"

extern void RunCliParseTests(CTestReport& cReport);
extern void RunPathUtilTests(CTestReport& cReport);

int main()
{
    CTestReport cReport;
    RunCliParseTests(cReport);
    RunPathUtilTests(cReport);
    return cReport.Finish();
}
