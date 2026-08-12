/**
 * @file test_task.cpp
 * @brief Unit tests for the download task state machine (P5).
 */

#include "task.h"
#include "test_framework.h"

/** @brief Test: legal/illegal state transitions and retry reset. */
static void TestTaskStateMachine(CTestReport& cReport)
{
    cReport.BeginCase("task: state machine");
    TDownloadTask tTask = {};
    tTask.emState = emTaskPending;

    BURST_EXPECT_TRUE(cReport, TaskCanStart(tTask) == TRUE);
    BURST_EXPECT_TRUE(cReport, TaskCanFinish(tTask, emTaskDone) == FALSE);
    TaskStart(tTask);
    BURST_EXPECT_TRUE(cReport, tTask.emState == emTaskRunning);
    BURST_EXPECT_TRUE(cReport, TaskCanStart(tTask) == FALSE);
    BURST_EXPECT_TRUE(cReport, TaskCanFinish(tTask, emTaskDone) == TRUE);
    BURST_EXPECT_TRUE(cReport, TaskCanFinish(tTask, emTaskPending) == FALSE);
    TaskFinish(tTask, emTaskDone);
    BURST_EXPECT_TRUE(cReport, tTask.emState == emTaskDone);
    BURST_EXPECT_TRUE(cReport, TaskCanFinish(tTask, emTaskError) == FALSE);

    /* Retry: terminal -> pending. */
    TaskResetForRetry(tTask);
    BURST_EXPECT_TRUE(cReport, tTask.emState == emTaskPending);
    TaskStart(tTask);
    TaskFinish(tTask, emTaskError);
    BURST_EXPECT_TRUE(cReport, tTask.emState == emTaskError);

    TaskResetForRetry(tTask);
    TaskStart(tTask);
    TaskFinish(tTask, emTaskCanceled);
    BURST_EXPECT_TRUE(cReport, tTask.emState == emTaskCanceled);
}

/** @brief Run all task state-machine tests. */
void RunTaskTests(CTestReport& cReport)
{
    TestTaskStateMachine(cReport);
}
