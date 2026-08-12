/**
 * @file test_threadpool.cpp
 * @brief Unit tests for the fixed-size worker thread pool (R12).
 */

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "test_framework.h"
#include "threadpool.h"

/** @brief Test: every submitted job runs exactly once. */
static void TestPoolRunsAllJobs(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: all jobs run");
    CThreadPool cPool(4u);
    std::atomic<u32> dwCount{0u};
    std::vector<std::future<void>> vecFutures;
    for (u32 dwIndex = 0; dwIndex < 64u; ++dwIndex)
    {
        std::future<void> fJob;
        BOOL32 bOk = cPool.Submit(
            [&dwCount]() { dwCount.fetch_add(1u, std::memory_order_relaxed); },
            &fJob);
        BURST_EXPECT_TRUE(cReport, bOk != FALSE);
        vecFutures.push_back(std::move(fJob));
    }
    for (std::future<void>& fJob : vecFutures)
    {
        fJob.wait();
    }
    BURST_EXPECT_TRUE(cReport, dwCount.load() == 64u);
    BURST_EXPECT_TRUE(cReport, cPool.PendingCount() == 0u);
}

/** @brief Test: a single worker is reused across sequential batches. */
static void TestPoolWorkerReuse(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: worker reuse");
    CThreadPool cPool(1u);
    std::set<std::thread::id> setIds;
    std::mutex mtx;
    for (u32 dwBatch = 0; dwBatch < 3u; ++dwBatch)
    {
        std::vector<std::future<void>> vecFutures;
        for (u32 dwIndex = 0; dwIndex < 5u; ++dwIndex)
        {
            std::future<void> fJob;
            cPool.Submit(
                [&setIds, &mtx]() {
                    std::lock_guard<std::mutex> lock(mtx);
                    setIds.insert(std::this_thread::get_id());
                },
                &fJob);
            vecFutures.push_back(std::move(fJob));
        }
        for (std::future<void>& fJob : vecFutures)
        {
            fJob.wait();
        }
    }
    BURST_EXPECT_TRUE(cReport, setIds.size() == 1u);
}

/** @brief Test: concurrent executions never exceed the worker count. */
static void TestPoolConcurrencyCap(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: concurrency cap");
    CThreadPool cPool(3u);
    std::atomic<u32> dwActive{0u};
    std::atomic<u32> dwMaxActive{0u};
    std::vector<std::future<void>> vecFutures;
    for (u32 dwIndex = 0; dwIndex < 9u; ++dwIndex)
    {
        std::future<void> fJob;
        cPool.Submit(
            [&dwActive, &dwMaxActive]() {
                u32 dwNow =
                    dwActive.fetch_add(1u, std::memory_order_relaxed) + 1u;
                u32 dwMax = dwMaxActive.load(std::memory_order_relaxed);
                while (dwNow > dwMax &&
                       !dwMaxActive.compare_exchange_weak(
                           dwMax, dwNow, std::memory_order_relaxed))
                {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                dwActive.fetch_sub(1u, std::memory_order_relaxed);
            },
            &fJob);
        vecFutures.push_back(std::move(fJob));
    }
    for (std::future<void>& fJob : vecFutures)
    {
        fJob.wait();
    }
    BURST_EXPECT_TRUE(cReport, dwMaxActive.load() == 3u);
}

/** @brief Test: future wait honors short timeouts and then completes. */
static void TestPoolWaitTimeout(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: future wait timeout");
    CThreadPool cPool(1u);
    std::future<void> fJob;
    cPool.Submit(
        []() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); },
        &fJob);
    BURST_EXPECT_TRUE(cReport, fJob.valid());
    BURST_EXPECT_TRUE(
        cReport, fJob.wait_for(std::chrono::milliseconds(20)) ==
                     std::future_status::timeout);
    BURST_EXPECT_TRUE(cReport, fJob.wait_for(std::chrono::seconds(2)) ==
                                   std::future_status::ready);
}

/** @brief Test: queued jobs drain on Shutdown; new submits are rejected. */
static void TestPoolRejectAfterShutdown(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: reject after shutdown");
    CThreadPool cPool(2u);
    std::atomic<u32> dwCount{0u};
    std::vector<std::future<void>> vecFutures;
    for (u32 dwIndex = 0; dwIndex < 3u; ++dwIndex)
    {
        std::future<void> fJob;
        cPool.Submit(
            [&dwCount]() { dwCount.fetch_add(1u, std::memory_order_relaxed); },
            &fJob);
        vecFutures.push_back(std::move(fJob));
    }
    cPool.Shutdown();
    for (std::future<void>& fJob : vecFutures)
    {
        fJob.wait();
    }
    BURST_EXPECT_TRUE(cReport, dwCount.load() == 3u);
    std::future<void> fRejected;
    BURST_EXPECT_TRUE(cReport, cPool.Submit([]() {}, &fRejected) == FALSE);
}

/** @brief Test: a throwing job does not kill the pool. */
static void TestPoolExceptionIsolation(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: exception isolation");
    CThreadPool cPool(2u);
    std::atomic<u32> dwCount{0u};
    std::vector<std::future<void>> vecFutures;
    for (u32 dwIndex = 0; dwIndex < 4u; ++dwIndex)
    {
        std::future<void> fJob;
        if (dwIndex == 0u)
        {
            cPool.Submit([]() { throw 42; });
        }
        else
        {
            cPool.Submit(
                [&dwCount]() {
                    dwCount.fetch_add(1u, std::memory_order_relaxed);
                },
                &fJob);
            vecFutures.push_back(std::move(fJob));
        }
    }
    for (std::future<void>& fJob : vecFutures)
    {
        fJob.wait();
    }
    BURST_EXPECT_TRUE(cReport, dwCount.load() == 3u);
}

/** @brief Test: pending count reflects queued plus running jobs. */
static void TestPoolPendingCount(CTestReport& cReport)
{
    cReport.BeginCase("threadpool: pending count");
    CThreadPool cPool(1u);
    std::vector<std::future<void>> vecFutures;
    for (u32 dwIndex = 0; dwIndex < 5u; ++dwIndex)
    {
        std::future<void> fJob;
        cPool.Submit(
            []() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); },
            &fJob);
        vecFutures.push_back(std::move(fJob));
    }
    /* One worker and five 30ms jobs: several must still be queued. */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    BURST_EXPECT_TRUE(cReport, cPool.PendingCount() > 1u);
    for (std::future<void>& fJob : vecFutures)
    {
        fJob.wait();
    }
    BURST_EXPECT_TRUE(cReport, cPool.PendingCount() == 0u);
}

/** @brief Run all thread-pool tests. */
void RunThreadPoolTests(CTestReport& cReport)
{
    TestPoolRunsAllJobs(cReport);
    TestPoolWorkerReuse(cReport);
    TestPoolConcurrencyCap(cReport);
    TestPoolWaitTimeout(cReport);
    TestPoolRejectAfterShutdown(cReport);
    TestPoolExceptionIsolation(cReport);
    TestPoolPendingCount(cReport);
}
