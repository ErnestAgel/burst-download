#ifndef BURST_WORKFLOW_H
#define BURST_WORKFLOW_H

/**
 * @file workflow.h
 * @brief Unified CLI execution flows (P9): single file / batch / video.
 *
 * The former main.cpp mixed argument parsing, naming and orchestration.
 * After P9 the entry point (src/entry/cli_main.cpp) parses arguments and
 * delegates the actual download execution to RunWorkflow.
 */

#include "cli_parse.h"

/**
 * @brief Run the download action selected by the parsed options.
 * @param tOpts Parsed CLI options (emAction must be emCliActionDownload;
 *              the caller guards the empty-URL / video-URL cases).
 * @return Process exit code (0 success, 1 failure, 3 partial batch success).
 */
int RunWorkflow(const TCliOptions& tOpts);

#endif  // BURST_WORKFLOW_H
