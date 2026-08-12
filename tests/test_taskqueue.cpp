/**
 * @file test_taskqueue.cpp
 * @brief Unit tests for the multi-task scheduler (P5/R12).
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "taskqueue.h"
#include "test_framework.h"
#include "threadpool.h"

/** @brief Test: every task reaches Done exactly once. */
static void TestQueueRunsAll(CTestReport& cReport)
{
    cReport.BeginCase("taskqueue: all tasks run");
    CThreadPool cPool(4u);
    CTaskQueue cQueue(2u, cPool);
    std::atomic<u32> dwCount{0u};
    for (u32 dwIndex = 0; dwIndex < 8u; ++dwIndex)
    {
        cQueue.AddTask("https://example.com/f" + std::to_string(dwIndex),
                       "", 1, 60, FALSE);
    }
    cQueue.Start(
        [&dwCount](TDownloadTask& tTask, CTaskContext& cCtx) -> BOOL32 {
            (void)tTask;
            (void)cCtx;
            dwCount.fetch_add(1u, std::memory_order_relaxed);
            return TRUE;
        });
    cQueue.WaitAll();
    BURST_EXPECT_TRUE(cReport, dwCount.load() == 8u);
    BURST_EXPECT_TRUE(cReport, cQueue.ActiveCount() == 0u);
    u32 dwDone = 0;
    for (const TDownloadTask& tTask : cQueue.Snapshot())
    {
        if (tTask.emState == emTaskDone)
        {
            ++dwDone;
        }
    }
    BURST_EXPECT_TRUE(cReport, dwDone == 8u);
}

/** @brief Test: concurrent executions never exceed the slot count. */
static void TestQueueConcurrencyCap(CTestReport& cReport)
{
    cReport.BeginCase("taskqueue: concurrency cap");
    CThreadPool cPool(4u);
    CTaskQueue cQueue(2u, cPool);
    std::atomic<u32> dwActive{0u};
    std::atomic<u32> dwMaxActive{0u};
    for (u32 dwIndex = 0; dwIndex < 6u; ++dwIndex)
    {
        cQueue.AddTask("https://example.com/c" + std::to_string(dwIndex),
                       "", 1, 60, FALSE);
    }
    cQueue.Start(
        [&dwActive, &dwMaxActive](TDownloadTask& tTask,
                                  CTaskContext& cCtx) -> BOOL32 {
            (void)tTask;
            (void)cCtx;
            const u32 dwNow =
                dwActive.fetch_add(1u, std::memory_order_relaxed) + 1u;
            u32 dwMax = dwMaxActive.load(std::memory_order_relaxed);
            while (dwNow > dwMax &&
                   !dwMaxActive.compare_exchange_weak(
                       dwMax, dwNow, std::memory_order_relaxed))
            {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            dwActive.fetch_sub(1u, std::memory_order_relaxed);
            return TRUE;
        });
    cQueue.WaitAll();
    BURST_EXPECT_TRUE(cReport, dwMaxActive.load() == 2u);
}

/** @brief Test: a queued (not yet running) task can be canceled. */
static void TestQueueCancelPending(CTestReport& cReport)
{
    cReport.BeginCase("taskqueue: cancel pending task");
    CThreadPool cPool(2u);
    CTaskQueue cQueue(1u, cPool);
    u64 dwCancelId = 0;
    for (u32 dwIndex = 0; dwIndex < 4u; ++dwIndex)
    {
        const u64 dwId = cQueue.AddTask(
            "https://example.com/x" + std::to_string(dwIndex), "", 1, 60,
            FALSE);
        if (dwIndex == 2u)
        {
            dwCancelId = dwId;
        }
    }
    cQueue.Start(
        [](TDownloadTask& tTask, CTaskContext& cCtx) -> BOOL32 {
            (void)tTask;
            (void)cCtx;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return TRUE;
        });
    cQueue.CancelTask(dwCancelId);
    cQueue.WaitAll();
    u32 dwCanceled = 0;
    for (const TDownloadTask& tTask : cQueue.Snapshot())
    {
        if (tTask.emState == emTaskCanceled)
        {
            ++dwCanceled;
        }
    }
    BURST_EXPECT_TRUE(cReport, dwCanceled == 1u);
}

/** @brief Test: a running task honors its cancellation context. */
static void TestQueueCancelRunning(CTestReport& cReport)
{
    cReport.BeginCase("taskqueue: cancel running task");
    CThreadPool cPool(2u);
    CTaskQueue cQueue(1u, cPool);
    const u64 dwRunId =
        cQueue.AddTask("https://example.com/r", "", 1, 60, FALSE);
    cQueue.Start(
        [](TDownloadTask& tTask, CTaskContext& cCtx) -> BOOL32 {
            (void)tTask;
            while (cCtx.IsCanceled() == FALSE)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return TRUE;
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cQueue.CancelTask(dwRunId);
    cQueue.WaitAll();
    const std::vector<TDownloadTask> vecTasks = cQueue.Snapshot();
    BURST_EXPECT_TRUE(cReport, vecTasks.size() == 1u);
    if (vecTasks.size() == 1u)
    {
        BURST_EXPECT_TRUE(cReport, vecTasks[0].emState == emTaskCanceled);
    }
}

/** @brief Test: save/load round trip and corrupt-file fallback. */
static void TestQueuePersistence(CTestReport& cReport)
{
    cReport.BeginCase("taskqueue: persistence round trip");
    const std::string strPath = "burst_queue_test.json";
    {
        CThreadPool cPool(2u);
        CTaskQueue cQueue(2u, cPool);
        cQueue.AddTask("https://example.com/a.iso", "a.iso", 4, 30, FALSE);
        cQueue.AddTask("https://example.com/b.iso", "b.iso", 2, 60, FALSE);
        BURST_EXPECT_TRUE(cReport, cQueue.Save(strPath) == TRUE);
    }
    {
        CThreadPool cPool(2u);
        CTaskQueue cQueue(2u, cPool);
        BURST_EXPECT_TRUE(cReport, cQueue.Load(strPath) == TRUE);
        const std::vector<TDownloadTask> vecTasks = cQueue.Snapshot();
        BURST_EXPECT_TRUE(cReport, vecTasks.size() == 2u);
        if (vecTasks.size() == 2u)
        {
            BURST_EXPECT_STR_EQ(cReport, "https://example.com/a.iso",
                                vecTasks[0].strUrl);
            BURST_EXPECT_STR_EQ(cReport, "b.iso", vecTasks[1].strOutput);
            BURST_EXPECT_TRUE(cReport, vecTasks[1].nThreads == 2);
        }

        std::ofstream fCorrupt(strPath.c_str(), std::ios::trunc);
        fCorrupt << "burst-queue-v1\nbad line\n";
        fCorrupt.close();
        BURST_EXPECT_TRUE(cReport, cQueue.Load(strPath) == FALSE);
        BURST_EXPECT_TRUE(cReport, cQueue.Snapshot().size() == 0u);
    }
    std::remove(strPath.c_str());
    std::remove((strPath + ".tmp").c_str());
}

/** @brief Run all task-queue tests. */
void RunTaskQueueTests(CTestReport& cReport)
{
    TestQueueRunsAll(cReport);
    TestQueueConcurrencyCap(cReport);
    TestQueueCancelPending(cReport);
    TestQueueCancelRunning(cReport);
    TestQueuePersistence(cReport);
}
