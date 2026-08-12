/**
 * @file threadpool.cpp
 * @brief Implementation of the fixed-size worker thread pool (R12).
 */

#include "threadpool.h"

CThreadPool::CThreadPool(u32 dwThreadCount)
    : m_dwNextId(1), m_dwOutstanding(0), m_bShutdown(FALSE)
{
    if (dwThreadCount < 1)
    {
        dwThreadCount = 1;
    }
    m_vecWorkers.reserve(dwThreadCount);
    for (u32 dwIndex = 0; dwIndex < dwThreadCount; ++dwIndex)
    {
        m_vecWorkers.emplace_back(&CThreadPool::WorkerLoop, this);
    }
}

CThreadPool::~CThreadPool()
{
    Shutdown();
}

BOOL32 CThreadPool::Submit(std::function<void()> fnJob,
                           std::future<void>* fJobOut)
{
    if (fnJob == nullptr)
    {
        return FALSE;
    }

    /* Wrap the body so the caller can wait on completion. */
    std::shared_ptr<std::packaged_task<void()>> pTask;
    if (fJobOut != nullptr)
    {
        pTask = std::make_shared<std::packaged_task<void()>>(std::move(fnJob));
        *fJobOut = pTask->get_future();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_bShutdown != FALSE)
    {
        return FALSE;
    }

    TQueuedJob job;
    job.dwId = m_dwNextId++;
    if (pTask != nullptr)
    {
        /* packaged_task captures exceptions into the future, so the
         * worker loop stays alive. */
        job.fnBody = [pTask]() { (*pTask)(); };
    }
    else
    {
        job.fnBody = std::move(fnJob);
    }

    m_deqPending.push_back(std::move(job));
    ++m_dwOutstanding;
    m_cvWork.notify_one();
    return TRUE;
}

u32 CThreadPool::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dwOutstanding;
}

u32 CThreadPool::ThreadCount() const
{
    return static_cast<u32>(m_vecWorkers.size());
}

void CThreadPool::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_bShutdown != FALSE)
        {
            return;
        }
        m_bShutdown = TRUE;
    }
    m_cvWork.notify_all();

    for (std::thread& thWorker : m_vecWorkers)
    {
        if (thWorker.joinable())
        {
            thWorker.join();
        }
    }
    m_vecWorkers.clear();
}

void CThreadPool::WorkerLoop()
{
    for (;;)
    {
        TQueuedJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cvWork.wait(lock, [this]() {
                return m_bShutdown != FALSE || !m_deqPending.empty();
            });
            if (m_bShutdown != FALSE && m_deqPending.empty())
            {
                break;  /* drained: no more jobs, exit the worker */
            }
            job = std::move(m_deqPending.front());
            m_deqPending.pop_front();
        }

        try
        {
            job.fnBody();
        }
        catch (...)
        {
            /* A job exception must never kill the pool. */
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_dwOutstanding > 0)
            {
                --m_dwOutstanding;
            }
        }
    }
}
