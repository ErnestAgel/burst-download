#ifndef BURST_TASKEXEC_H
#define BURST_TASKEXEC_H

/**
 * @file taskexec.h
 * @brief Unified single-task executor shared by the CLI and the GUI.
 *
 * The CLI and the GUI previously carried two copies of the file/video
 * download orchestration.  TaskExecRun is the single implementation:
 * callers provide options (URL, output, threads, cookie, verify...) and
 * callbacks (stage/progress/log/cancel); the executor reports through the
 * callbacks and returns a boolean plus an error string.
 */

#include <functional>
#include <string>
#include <vector>

#include "burst_types.h"
#include "../src/progress.h"

class CThreadPool;

/** @brief Options for one download task (file or video). */
typedef struct tagTaskExecOptions
{
    std::string   strUrl;
    std::string   strOutput;      /**< file path or video base name */
    std::string   strCookie;
    std::string   strCookiesFromBrowser;
    s32           nThreads;
    s32           nTimeout;
    BOOL32        bVideo;
    BOOL32        bVerifySha256;
    BOOL32        bDeletePartial;
    CThreadPool*  pChunkPool;     /**< shared download pool (P8), may be null */
} TTaskExecOptions;

/** @brief Callbacks used by the executor to report progress and events. */
typedef struct tagTaskExecCallbacks
{
    std::function<void(int)> fnOnStage;
    std::function<void(const std::vector<ThreadProgress>&, double, double)>
        fnOnProgress;
    std::function<void(const std::string&)> fnOnLog;
    std::function<BOOL32()> fnIsCanceled;
} TTaskExecCallbacks;

/**
 * @brief Run one file or video download task.
 * @param tOpts Task options (output path must already be resolved).
 * @param tCb   Reporting callbacks (each may be empty).
 * @param strOutputPath Receives the final output path (video merge result).
 * @param strError      Failure reason on FALSE.
 * @return TRUE on success.
 */
BOOL32 TaskExecRun(const TTaskExecOptions& tOpts, TTaskExecCallbacks& tCb,
                   std::string& strOutputPath, std::string& strError);

#endif  // BURST_TASKEXEC_H
