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

/** @brief Test: the cancel flag can be polled through CancelFlagPtr (the
 *         shared flag the download engines poll, P8-4). */
static void TestTaskCancelFlagPtr(CTestReport& cReport)
{
    cReport.BeginCase("task: cancel flag pointer");
    CTaskContext cCtx;
    BURST_EXPECT_TRUE(cReport, cCtx.IsCanceled() == FALSE);

    std::atomic<bool>* pFlag = cCtx.CancelFlagPtr();
    BURST_EXPECT_TRUE(cReport, pFlag != nullptr);
    BURST_EXPECT_TRUE(cReport, pFlag->load() == false);

    cCtx.Cancel();
    BURST_EXPECT_TRUE(cReport, pFlag->load() == true);
    BURST_EXPECT_TRUE(cReport, cCtx.IsCanceled() == TRUE);

    cCtx.Reset();
    BURST_EXPECT_TRUE(cReport, pFlag->load() == false);
    BURST_EXPECT_TRUE(cReport, cCtx.IsCanceled() == FALSE);
}

/** @brief Run all task state-machine tests. */
void RunTaskTests(CTestReport& cReport)
{
    TestTaskStateMachine(cReport);
    TestTaskCancelFlagPtr(cReport);
}
