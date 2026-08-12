/**
 * @file curlmulti.cpp
 * @brief Shared curl_multi transfer engine implementation (P8-4).
 */

#include "curlmulti.h"

#include <utility>

namespace
{

/** @brief Poll timeout per driver cycle (ms); keeps cancel/queue latency
 *         bounded while transfers are idle. */
static const long kLanePollTimeoutMs = 100L;

/** @brief Invoke a completion callback when present. */
static void InvokeDone(const std::function<void(CURLcode, long)>& fnDone,
                       CURLcode code, long lHttpCode)
{
    if (fnDone)
    {
        fnDone(code, lHttpCode);
    }
}

}  // namespace

CCurlMultiEngine::CCurlMultiEngine(u32 dwLaneCount)
{
    if (dwLaneCount < 1u)
    {
        dwLaneCount = 1u;
    }

    /* libcurl global init must finish before any other thread touches the
     * library (the engine spawns driver threads in the constructor). */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    m_vecLanes.reserve(dwLaneCount);
    for (u32 dwIndex = 0; dwIndex < dwLaneCount; ++dwIndex)
    {
        std::unique_ptr<TLaneState> pLane = std::make_unique<TLaneState>();
        pLane->pMulti = curl_multi_init();
        if (pLane->pMulti == nullptr)
        {
            /* curl_multi_init failure is unrecoverable; keep the lane alive
             * with a null handle so driver loops simply drain queues. */
        }
        m_vecLanes.push_back(std::move(pLane));
    }

    for (u32 dwIndex = 0; dwIndex < m_vecLanes.size(); ++dwIndex)
    {
        m_vecLanes[dwIndex]->thDriver =
            std::thread(&CCurlMultiEngine::DriverLoop, this, dwIndex);
    }
}

CCurlMultiEngine::~CCurlMultiEngine()
{
    Shutdown();
}

BOOL32 CCurlMultiEngine::SubmitChunk(TChunkJob& tJob)
{
    if (!tJob.fnCreateEasy || !tJob.fnDone)
    {
        return FALSE;
    }
    if (m_bShutdown.load() != FALSE)
    {
        return FALSE;
    }
    if (tJob.dwLane >= m_vecLanes.size())
    {
        tJob.dwLane = m_dwNextLane.fetch_add(1u) % m_vecLanes.size();
    }

    TLaneState& tLane = *m_vecLanes[tJob.dwLane];
    std::lock_guard<std::mutex> lock(tLane.mutex);
    if (m_bShutdown.load() != FALSE)
    {
        return FALSE;
    }
    tLane.deqPending.push_back(std::move(tJob));
    return TRUE;
}

BOOL32 CCurlMultiEngine::SubmitChunkDelayed(TChunkJob& tJob, u64 dwDelayMs)
{
    if (!tJob.fnCreateEasy || !tJob.fnDone)
    {
        return FALSE;
    }
    if (m_bShutdown.load() != FALSE)
    {
        return FALSE;
    }
    if (tJob.dwLane >= m_vecLanes.size())
    {
        return FALSE;  /* the original submit must have assigned a lane */
    }

    TLaneState& tLane = *m_vecLanes[tJob.dwLane];
    const std::chrono::steady_clock::time_point tReady =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(dwDelayMs);
    std::lock_guard<std::mutex> lock(tLane.mutex);
    if (m_bShutdown.load() != FALSE)
    {
        return FALSE;
    }
    tLane.deqRetry.push_back(std::make_pair(tReady, std::move(tJob)));
    return TRUE;
}

u32 CCurlMultiEngine::LaneCount() const
{
    return static_cast<u32>(m_vecLanes.size());
}

u32 CCurlMultiEngine::ActiveCount() const
{
    u32 dwTotal = 0u;
    for (const std::unique_ptr<TLaneState>& pLane : m_vecLanes)
    {
        std::lock_guard<std::mutex> lock(pLane->mutex);
        dwTotal += static_cast<u32>(pLane->deqPending.size());
        dwTotal += static_cast<u32>(pLane->deqRetry.size());
        dwTotal += static_cast<u32>(pLane->mapActive.size());
    }
    return dwTotal;
}

void CCurlMultiEngine::Shutdown()
{
    BOOL32 bExpected = FALSE;
    if (m_bShutdown.compare_exchange_strong(bExpected, TRUE) == FALSE)
    {
        return;  /* already shutting down */
    }

    for (std::unique_ptr<TLaneState>& pLane : m_vecLanes)
    {
        if (pLane->thDriver.joinable())
        {
            pLane->thDriver.join();
        }
        if (pLane->pMulti != nullptr)
        {
            curl_multi_cleanup(pLane->pMulti);
            pLane->pMulti = nullptr;
        }
    }
}

void CCurlMultiEngine::DriverLoop(u32 dwLane)
{
    TLaneState& tLane = *m_vecLanes[dwLane];

    for (;;)
    {
        std::vector<TChunkJob> vecStart;
        std::vector<TChunkJob> vecCanceled;

        /* 1. Move due retries into pending, then drain pending into
         *    start/canceled buckets (never invoke callbacks under lock). */
        {
            std::unique_lock<std::mutex> lock(tLane.mutex);
            const std::chrono::steady_clock::time_point tNow =
                std::chrono::steady_clock::now();
            while ((tLane.deqRetry.empty() == false) &&
                   (tLane.deqRetry.front().first <= tNow))
            {
                TChunkJob tJob = std::move(tLane.deqRetry.front().second);
                tLane.deqRetry.pop_front();
                tLane.deqPending.push_back(std::move(tJob));
            }
            /* Shutdown must also drain retries that are not due yet. */
            if (m_bShutdown.load() != FALSE)
            {
                while (tLane.deqRetry.empty() == false)
                {
                    TChunkJob tJob = std::move(tLane.deqRetry.front().second);
                    tLane.deqRetry.pop_front();
                    tLane.deqPending.push_back(std::move(tJob));
                }
            }
            while (tLane.deqPending.empty() == false)
            {
                TChunkJob tJob = std::move(tLane.deqPending.front());
                tLane.deqPending.pop_front();
                const BOOL32 bCanceled =
                    (tJob.pCancelFlag != nullptr) &&
                    (tJob.pCancelFlag->load() != FALSE);
                if ((m_bShutdown.load() != FALSE) || (bCanceled != FALSE))
                {
                    vecCanceled.push_back(std::move(tJob));
                }
                else
                {
                    vecStart.push_back(std::move(tJob));
                }
            }
        }

        /* 2. Cancel active transfers whose flag is set (or on shutdown);
         *    the active map is owned by this driver thread only. */
        std::vector<std::shared_ptr<TChunkJob> > vecAborted;
        if ((tLane.pMulti != nullptr) && (tLane.mapActive.empty() == false))
        {
            for (std::map<CURL*, std::shared_ptr<TChunkJob> >::iterator it =
                     tLane.mapActive.begin();
                 it != tLane.mapActive.end();)
            {
                const TChunkJob& tJob = *it->second;
                const BOOL32 bCanceled =
                    (tJob.pCancelFlag != nullptr) &&
                    (tJob.pCancelFlag->load() != FALSE);
                if ((m_bShutdown.load() != FALSE) || (bCanceled != FALSE))
                {
                    CURL* pEasy = it->first;
                    curl_multi_remove_handle(tLane.pMulti, pEasy);
                    curl_easy_cleanup(pEasy);
                    vecAborted.push_back(std::move(it->second));
                    it = tLane.mapActive.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        /* 3. Start queued jobs (easy handles are created and used only on
         *    this lane, so no cross-thread CURL access occurs). */
        for (TChunkJob& tJob : vecStart)
        {
            CURL* pEasy = tJob.fnCreateEasy();
            if (pEasy == nullptr)
            {
                InvokeDone(tJob.fnDone, CURLE_ABORTED_BY_CALLBACK, 0L);
                continue;
            }
            if (tLane.pMulti == nullptr)
            {
                curl_easy_cleanup(pEasy);
                InvokeDone(tJob.fnDone, CURLE_FAILED_INIT, 0L);
                continue;
            }
            std::shared_ptr<TChunkJob> pJob =
                std::make_shared<TChunkJob>(std::move(tJob));
            const CURLMcode mcode = curl_multi_add_handle(tLane.pMulti, pEasy);
            if (mcode != CURLM_OK)
            {
                curl_easy_cleanup(pEasy);
                InvokeDone(pJob->fnDone, CURLE_FAILED_INIT, 0L);
                continue;
            }
            tLane.mapActive.emplace(pEasy, pJob);
        }

        /* 4. Drive the multi handle (non-blocking). */
        if (tLane.pMulti != nullptr)
        {
            int nRunning = 0;
            curl_multi_perform(tLane.pMulti, &nRunning);

            /* 5. Finalize completed transfers. */
            int nMsgsLeft = 0;
            CURLMsg* pMsg = nullptr;
            while ((pMsg = curl_multi_info_read(tLane.pMulti, &nMsgsLeft)) !=
                   nullptr)
            {
                if (pMsg->msg != CURLMSG_DONE)
                {
                    continue;
                }
                CURL* pEasy = pMsg->easy_handle;
                const CURLcode code = pMsg->data.result;
                long lHttpCode = 0L;
                curl_easy_getinfo(pEasy, CURLINFO_RESPONSE_CODE, &lHttpCode);
                std::shared_ptr<TChunkJob> pJob;
                std::map<CURL*, std::shared_ptr<TChunkJob> >::iterator it =
                    tLane.mapActive.find(pEasy);
                if (it != tLane.mapActive.end())
                {
                    pJob = it->second;
                    tLane.mapActive.erase(it);
                }
                curl_multi_remove_handle(tLane.pMulti, pEasy);
                curl_easy_cleanup(pEasy);
                if (pJob != nullptr)
                {
                    InvokeDone(pJob->fnDone, code, lHttpCode);
                }
            }
        }

        /* 6. Report canceled/aborted jobs after the perform cycle. */
        for (const std::shared_ptr<TChunkJob>& pJob : vecAborted)
        {
            InvokeDone(pJob->fnDone, CURLE_ABORTED_BY_CALLBACK, 0L);
        }
        for (TChunkJob& tJob : vecCanceled)
        {
            InvokeDone(tJob.fnDone, CURLE_ABORTED_BY_CALLBACK, 0L);
        }

        /* 7. Exit when shutdown and every queue is drained. */
        {
            std::lock_guard<std::mutex> lock(tLane.mutex);
            if ((m_bShutdown.load() != FALSE) &&
                tLane.deqPending.empty() && tLane.deqRetry.empty() &&
                tLane.mapActive.empty())
            {
                break;
            }
        }

        /* 8. Wait for socket activity; the short timeout keeps the loop
         *    responsive to new jobs and cancellation requests.  When no
         *    transfer is active, sleep explicitly so an idle engine never
         *    busy-loops. */
        if ((tLane.pMulti != nullptr) && (tLane.mapActive.empty() == false))
        {
            int nFds = 0;
            curl_multi_wait(tLane.pMulti, nullptr, 0, kLanePollTimeoutMs,
                            &nFds);
        }
        else
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kLanePollTimeoutMs));
        }
    }
}
