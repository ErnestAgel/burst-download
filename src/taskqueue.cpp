/**
 * @file taskqueue.cpp
 * @brief Multi-task scheduler implementation (P5, R12).
 */

#include "taskqueue.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{

const char* kQueueHeader = "burst-queue-v1";

/** @brief Escape tab/newline/backslash so fields stay single-line. */
std::string EscapeField(const std::string& strIn)
{
    std::string strOut;
    strOut.reserve(strIn.size());
    for (char c : strIn)
    {
        if (c == '\\')
        {
            strOut += "\\\\";
        }
        else if (c == '\t')
        {
            strOut += "\\t";
        }
        else if (c == '\n')
        {
            strOut += "\\n";
        }
        else if (c == '\r')
        {
            strOut += "\\r";
        }
        else
        {
            strOut += c;
        }
    }
    return strOut;
}

/** @brief Reverse EscapeField; sets bOk FALSE on invalid escape sequences. */
std::string UnescapeField(const std::string& strIn, BOOL32& bOk)
{
    std::string strOut;
    strOut.reserve(strIn.size());
    bOk = TRUE;
    for (size_t nIndex = 0; nIndex < strIn.size(); ++nIndex)
    {
        const char c = strIn[nIndex];
        if (c != '\\')
        {
            strOut += c;
            continue;
        }
        if (nIndex + 1 >= strIn.size())
        {
            bOk = FALSE;
            return strOut;
        }
        const char cNext = strIn[++nIndex];
        if (cNext == 't')
        {
            strOut += '\t';
        }
        else if (cNext == 'n')
        {
            strOut += '\n';
        }
        else if (cNext == 'r')
        {
            strOut += '\r';
        }
        else if (cNext == '\\')
        {
            strOut += '\\';
        }
        else
        {
            bOk = FALSE;
            return strOut;
        }
    }
    return strOut;
}

/** @brief Split a line into tab-separated fields. */
void SplitFields(const std::string& strLine, std::vector<std::string>& vecOut)
{
    std::istringstream ss(strLine);
    std::string strField;
    while (std::getline(ss, strField, '\t'))
    {
        vecOut.push_back(strField);
    }
}

}  // namespace

CTaskQueue::CTaskQueue(u32 dwSlots, CThreadPool& cPool)
    : m_cPool(cPool), m_dwSlots(dwSlots < 1 ? 1 : dwSlots), m_dwNextId(1),
      m_bStarted(FALSE), m_dwRunning(0)
{
}

CTaskQueue::~CTaskQueue()
{
    if (m_bStarted != FALSE)
    {
        WaitAll();
    }
}

u64 CTaskQueue::AddTask(const std::string& strUrl, const std::string& strOutput,
                        s32 nThreads, s32 nTimeout, BOOL32 bVideoMode)
{
    if (strUrl.empty())
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const u64 dwId = m_dwNextId++;
    std::shared_ptr<TQueueEntry> pEntry = std::make_shared<TQueueEntry>();
    pEntry->tTask.dwId = dwId;
    pEntry->tTask.strUrl = strUrl;
    pEntry->tTask.strOutput = strOutput;
    pEntry->tTask.nThreads = nThreads;
    pEntry->tTask.nTimeout = nTimeout;
    pEntry->tTask.bVideoMode = bVideoMode;
    pEntry->tTask.emState = emTaskPending;
    pEntry->tTask.nCreatedAt = static_cast<s64>(time(nullptr));
    pEntry->tTask.nFinishedAt = 0;
    m_mapEntries.emplace(dwId, pEntry);
    DispatchLocked();
    return dwId;
}

void CTaskQueue::Start(TTaskExecutor fnExecutor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_bStarted != FALSE)
    {
        return;
    }
    m_fnExecutor = std::move(fnExecutor);
    m_bStarted = TRUE;
    DispatchLocked();
}

void CTaskQueue::CancelTask(u64 dwId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<u64, std::shared_ptr<TQueueEntry> >::iterator it =
        m_mapEntries.find(dwId);
    if (it == m_mapEntries.end())
    {
        return;
    }
    if (it->second->tTask.emState == emTaskPending)
    {
        it->second->tTask.emState = emTaskCanceled;
        it->second->tTask.nFinishedAt = static_cast<s64>(time(nullptr));
        m_cvIdle.notify_all();
    }
    else if (it->second->tTask.emState == emTaskRunning)
    {
        it->second->cCtx.Cancel();
    }
}

void CTaskQueue::CancelAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (std::map<u64, std::shared_ptr<TQueueEntry> >::iterator it =
             m_mapEntries.begin();
         it != m_mapEntries.end(); ++it)
    {
        if (it->second->tTask.emState == emTaskPending)
        {
            it->second->tTask.emState = emTaskCanceled;
            it->second->tTask.nFinishedAt =
                static_cast<s64>(time(nullptr));
        }
        else if (it->second->tTask.emState == emTaskRunning)
        {
            it->second->cCtx.Cancel();
        }
    }
    m_cvIdle.notify_all();
}

void CTaskQueue::WaitAll()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cvIdle.wait(lock, [this]() {
        for (std::map<u64, std::shared_ptr<TQueueEntry> >::const_iterator it =
                 m_mapEntries.begin();
             it != m_mapEntries.end(); ++it)
        {
            const TTaskState em = it->second->tTask.emState;
            if ((em == emTaskPending) || (em == emTaskRunning))
            {
                return false;
            }
        }
        return true;
    });
}

std::vector<TDownloadTask> CTaskQueue::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TDownloadTask> vecOut;
    vecOut.reserve(m_mapEntries.size());
    for (std::map<u64, std::shared_ptr<TQueueEntry> >::const_iterator it =
             m_mapEntries.begin();
         it != m_mapEntries.end(); ++it)
    {
        vecOut.push_back(it->second->tTask);
    }
    return vecOut;
}

u32 CTaskQueue::Slots() const
{
    return m_dwSlots;
}

u32 CTaskQueue::ActiveCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    u32 dwActive = 0;
    for (std::map<u64, std::shared_ptr<TQueueEntry> >::const_iterator it =
             m_mapEntries.begin();
         it != m_mapEntries.end(); ++it)
    {
        const TTaskState em = it->second->tTask.emState;
        if ((em == emTaskPending) || (em == emTaskRunning))
        {
            ++dwActive;
        }
    }
    return dwActive;
}

BOOL32 CTaskQueue::Save(const std::string& strPath) const
{
    const std::string strTmp = strPath + ".tmp";
    std::ofstream fOut(strTmp.c_str(), std::ios::trunc);
    if (!fOut.is_open())
    {
        return FALSE;
    }

    fOut << kQueueHeader << "\n";
    const std::vector<TDownloadTask> vecTasks = Snapshot();
    for (const TDownloadTask& tTask : vecTasks)
    {
        fOut << tTask.dwId << '\t'
             << EscapeField(tTask.strUrl) << '\t'
             << EscapeField(tTask.strOutput) << '\t'
             << tTask.nThreads << '\t'
             << tTask.nTimeout << '\t'
             << (tTask.bVideoMode != FALSE ? 1 : 0) << '\t'
             << static_cast<s32>(tTask.emState) << '\t'
             << EscapeField(tTask.strError) << '\t'
             << tTask.nCreatedAt << '\t'
             << tTask.nFinishedAt << '\n';
    }
    fOut.close();
    if (!fOut.good())
    {
        std::remove(strTmp.c_str());
        return FALSE;
    }

    std::error_code ec;
    std::filesystem::rename(strTmp, strPath, ec);
    if (ec)
    {
        /* Windows rename may fail when the target exists; retry after
         * removing the target. */
        std::remove(strPath.c_str());
        if (std::rename(strTmp.c_str(), strPath.c_str()) != 0)
        {
            std::remove(strTmp.c_str());
            return FALSE;
        }
    }
    return TRUE;
}

BOOL32 CTaskQueue::Load(const std::string& strPath)
{
    std::ifstream fIn(strPath.c_str());
    if (!fIn.is_open())
    {
        return FALSE;
    }

    std::string strHeader;
    if (!std::getline(fIn, strHeader) || (strHeader != kQueueHeader))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mapEntries.clear();
        return FALSE;
    }

    std::map<u64, std::shared_ptr<TQueueEntry> > mapLoaded;
    u64 dwMaxId = 0;
    BOOL32 bParseOk = TRUE;
    std::string strLine;
    while ((bParseOk == TRUE) && std::getline(fIn, strLine))
    {
        if (strLine.empty())
        {
            continue;
        }
        std::vector<std::string> vecFields;
        SplitFields(strLine, vecFields);
        if (vecFields.size() < 10)
        {
            bParseOk = FALSE;
            break;
        }

        std::shared_ptr<TQueueEntry> pEntry =
            std::make_shared<TQueueEntry>();
        BOOL32 bOk = FALSE;
        pEntry->tTask.dwId = static_cast<u64>(std::atoll(vecFields[0].c_str()));
        pEntry->tTask.strUrl = UnescapeField(vecFields[1], bOk);
        if (bOk == FALSE)
        {
            bParseOk = FALSE;
            break;
        }
        pEntry->tTask.strOutput = UnescapeField(vecFields[2], bOk);
        if (bOk == FALSE)
        {
            bParseOk = FALSE;
            break;
        }
        pEntry->tTask.nThreads = static_cast<s32>(std::atoi(vecFields[3].c_str()));
        pEntry->tTask.nTimeout = static_cast<s32>(std::atoi(vecFields[4].c_str()));
        pEntry->tTask.bVideoMode =
            (std::atoi(vecFields[5].c_str()) != 0) ? TRUE : FALSE;
        pEntry->tTask.emState =
            static_cast<TTaskState>(std::atoi(vecFields[6].c_str()));
        pEntry->tTask.strError = UnescapeField(vecFields[7], bOk);
        if (bOk == FALSE)
        {
            bParseOk = FALSE;
            break;
        }
        pEntry->tTask.nCreatedAt =
            static_cast<s64>(std::atoll(vecFields[8].c_str()));
        pEntry->tTask.nFinishedAt =
            static_cast<s64>(std::atoll(vecFields[9].c_str()));
        mapLoaded.emplace(pEntry->tTask.dwId, pEntry);
        if (pEntry->tTask.dwId > dwMaxId)
        {
            dwMaxId = pEntry->tTask.dwId;
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (bParseOk == FALSE)
    {
        m_mapEntries.clear();
        return FALSE;
    }
    m_mapEntries.swap(mapLoaded);
    if (dwMaxId >= m_dwNextId)
    {
        m_dwNextId = dwMaxId + 1;
    }
    return TRUE;
}

void CTaskQueue::DispatchLocked()
{
    if (m_bStarted == FALSE)
    {
        return;
    }
    for (;;)
    {
        std::shared_ptr<TQueueEntry> pEntry;
        for (std::map<u64, std::shared_ptr<TQueueEntry> >::iterator it =
                 m_mapEntries.begin();
             it != m_mapEntries.end(); ++it)
        {
            if (it->second->tTask.emState == emTaskPending)
            {
                pEntry = it->second;
                break;
            }
        }
        if ((pEntry == nullptr) || (m_dwRunning >= m_dwSlots))
        {
            break;
        }

        TaskStart(pEntry->tTask);
        ++m_dwRunning;
        if (m_cPool.Submit([this, pEntry]() { RunTask(pEntry); }) == FALSE)
        {
            TaskFinish(pEntry->tTask, emTaskError);
            if (m_dwRunning > 0)
            {
                --m_dwRunning;
            }
            m_cvIdle.notify_all();
            break;
        }
    }
}

void CTaskQueue::RunTask(std::shared_ptr<TQueueEntry> pEntry)
{
    BOOL32 bOk = FALSE;
    if (m_fnExecutor != nullptr)
    {
        bOk = m_fnExecutor(pEntry->tTask, pEntry->cCtx);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (pEntry->cCtx.IsCanceled() == TRUE)
    {
        TaskFinish(pEntry->tTask, emTaskCanceled);
    }
    else
    {
        TaskFinish(pEntry->tTask,
                   (bOk != FALSE) ? emTaskDone : emTaskError);
    }
    if (m_dwRunning > 0)
    {
        --m_dwRunning;
    }
    m_cvIdle.notify_all();
    DispatchLocked();
}
