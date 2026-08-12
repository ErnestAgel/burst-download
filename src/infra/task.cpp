/**
 * @file task.cpp
 * @brief Download task state machine implementation.
 */

#include "task.h"

#include <ctime>

BOOL32 TaskCanStart(const TDownloadTask& tTask)
{
    return tTask.emState == emTaskPending;
}

BOOL32 TaskCanFinish(const TDownloadTask& tTask, TTaskState emNext)
{
    if (tTask.emState != emTaskRunning)
    {
        return FALSE;
    }
    return (emNext == emTaskDone) || (emNext == emTaskError) ||
           (emNext == emTaskCanceled);
}

void TaskStart(TDownloadTask& tTask)
{
    if (TaskCanStart(tTask) == FALSE)
    {
        return;
    }
    tTask.emState = emTaskRunning;
    tTask.nFinishedAt = 0;
}

void TaskFinish(TDownloadTask& tTask, TTaskState emNext)
{
    if (TaskCanFinish(tTask, emNext) == FALSE)
    {
        return;
    }
    tTask.emState = emNext;
    tTask.nFinishedAt = static_cast<s64>(time(nullptr));
}

void TaskResetForRetry(TDownloadTask& tTask)
{
    tTask.emState = emTaskPending;
    tTask.strError = "";
    tTask.nFinishedAt = 0;
}
