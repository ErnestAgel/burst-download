/**
 * @file Ccurl.h
 * @brief Multi-threaded chunked downloader C++ wrapper declaration based on
 *        libcurl (cross-platform: Windows / Linux x86_64 / Linux aarch64).
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <iostream>
#include <curl/curl.h>
#include <string>
#include <mutex>
#include <vector>
#include <atomic>
#include <functional>
#include <ctime>
#include <thread>
#include <algorithm>

#include "../src/progress.h"
#include "burst_types.h"

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace std;

/** @brief Maximum number of chunk download threads (-t upper limit). */
/* Hard compile-time cap (chunk table array size); the practical cap adapts
 * to the CPU core count (BurstMaxThreads, never above 8). */
#define MaxThread 8

/* ---- Adaptive thread strategy (runtime CPU-core aware) ----
 * Fall back to 8 when core detection fails;
 * cap = clamp(cores, 4, 8): at most 8 connections on any machine (IDM
 * default tier, polite to CDNs/servers; beyond 8 gains shrink and rate
 * limits are more likely);
 * default = clamp(cores, 2, 4): 4 connections on mainstream machines, 2 on
 * 1~2-core weak machines. */
inline int BurstLogicalCores() {
  unsigned n = std::thread::hardware_concurrency();
  return n > 0 ? static_cast<int>(n) : 8;
}
inline int BurstMaxThreads() {
  return std::min(std::max(BurstLogicalCores(), 4), 8);
}
inline int BurstDefaultThreads() {
  return std::min(std::max(BurstLogicalCores(), 2), 4);
}

class Ccurl;

/**
 * @brief Per-chunk download task information.
 */
typedef struct tagEasyList {
    const char* url;       /**< URL of the chunk's download */
    char* file_ptr;        /**< Mapped file memory start (mmap/MapViewOfFile) */
    int64_t offset;        /**< Current write offset (advances while writing) */
    int64_t end;           /**< Chunk end offset */
#ifdef _WIN32
    HANDLE thid;           /**< Download thread handle (Windows) */
#else
    pthread_t thid;        /**< Download thread ID (Linux) */
#endif
    double download_len;   /**< Bytes downloaded by this chunk */
    bool use_range;        /**< Whether the server supports HTTP Range */
    bool success;          /**< Whether this chunk finished */
    bool thread_created;   /**< Whether the chunk thread was created */
    long timeout;          /**< Low-speed timeout seconds (0 = disabled) */
    const char* referer;   /**< Anti-hotlink Referer (may be empty) */
    const char* cookie;    /**< Cookie string (may be empty) */
    /* ---- GUI progress/cancel extension ---- */
    int64_t part_start;    /**< Initial chunk start (resume base) */
    int64_t part_total;    /**< Chunk length (end - start + 1) */
    double  last_len;      /**< Last progress callback download amount */
    time_t  last_t;        /**< Last progress callback time */
    std::atomic<bool>* cancel_flag;  /**< Owner cancel flag */
    const std::function<void(const std::vector<ThreadProgress>&,
                             double, double)>* on_progress;
    /* ---- P2: strict 206 + single-stream degrade ---- */
    BOOL32 bGot206;                 /**< Final response of this chunk is 206 */
    std::atomic<bool>* range_denied; /**< Set when server ignored Range (200) */
    Ccurl* owner;                   /**< Owning Ccurl instance */
} st_EasyList;

/**
 * @brief Multi-threaded chunked downloader (cross-platform).
 */
class Ccurl
{
public:
    /**
     * @brief Constructor: initializes the chunk table and prints the libcurl
     *        version.
     */
    Ccurl();

    /**
     * @brief Destructor: releases chunks and the memory mapping.
     */
    ~Ccurl();

    /**
     * @brief Initialize a download task: save URL/file name, probe Range
     *        support and create the local file.
     * @param url Download URL.
     * @param filename Local file name.
     * @param thread_num Chunk threads (1 ~ MaxThread; auto-degrades to 1
     *        when the server does not support Range).
     * @param timeout Low-speed timeout seconds (default 60; 0 = disabled).
     * @return TRUE when initialization succeeded.
     */
    bool Init(const string url, string filename,
              int thread_num = MaxThread, int timeout = 60);

    /**
     * @brief Start the multi-threaded chunked download.
     * @return TRUE when every chunk succeeded.
     */
    bool Download_Task();

    /**
     * @brief Reserved upload interface.
     * @param server_url Server URL.
     * @return TRUE when the upload succeeded.
     */
    bool Uploading_Task(const char* server_url);

    /** @brief Set the anti-hotlink Referer. */
    void SetReferer(const string& referer);

    /** @brief Set the request Cookie. */
    void SetCookie(const string& cookie);

    /** @brief Request cancellation (write/progress callback checkpoints). */
    void Cancel();

    /** @brief Whether cancellation was requested. */
    bool IsCanceled() const;

    /** @brief Chunk progress placeholders before the first callback. */
    std::vector<ThreadProgress> SnapshotParts() const;

    /** @brief Most recent failure reason (UTF-8; empty when none). */
    std::string LastError() const;

    /**
     * @brief Progress callback (GUI injects it; ~200ms throttle).
     */
    std::function<void(const std::vector<ThreadProgress>&, double, double)>
        onProgress;

    /**
     * @brief Thread entry: download one chunk.
     */
    static void* Downloading(void* arg);

    /**
     * @brief Flush dirty mapped pages to disk (crash consistency, issue R4).
     * @note No locking: called from transfer threads via the owner pointer.
     */
    void FlushMapping();

    /**
     * @brief Compute the SHA-256 digest of the downloaded file (issue O4).
     * @param strDigest Output lowercase hex digest.
     * @return TRUE on success (files over 4 GiB are rejected).
     */
    bool VerifySha256(std::string& strDigest);

    st_EasyList *m_Easy_List[MaxThread + 1];  /**< Chunk task table */

private:
    /** @brief Resume meta load result. */
    typedef enum tagMetaResult {
        emMetaNone = 0,   /**< No usable meta */
        emMetaOk,         /**< Meta usable, integrity matches */
        emMetaChanged     /**< Meta invalid / remote content changed */
    } TMetaResult;

    /** @brief Probe the total file size (HEAD first, then Range GET). */
    bool get_Download_FileSize();

    /** @brief Detect HTTP Range support (GET + Range: 0-0, expects 206). */
    bool Check_Range_Support();

    /** @brief Initialize the local file: resume check, create, mmap,
     *         chunk the range. */
    bool File_Init(const char* filename);

    /** @brief Create and join chunk threads; return whether all succeeded. */
    bool RunChunks();

    /** @brief Verify every chunk wrote its full range. */
    bool VerifyAllPartsWritten() const;

    /** @brief Release chunks, unmap and close the file (no locking). */
    void DestoryUnlocked();

    /** @brief Release chunks, unmap and close the file. */
    void Destory();

    void* m_easyHandle = nullptr;   /**< libcurl easy handle */
    string m_filename;              /**< Local file name */
    string m_url;                   /**< Download URL */
    std::mutex m_lock;              /**< Lifecycle mutex */
    char* m_pTrunck = nullptr;      /**< Mapped memory address */
#ifdef _WIN32
    HANDLE m_hFile = INVALID_HANDLE_VALUE;   /**< File handle (Windows) */
    HANDLE m_hMapping = nullptr;             /**< File mapping handle */
#else
    int m_fd = -1;                  /**< File descriptor (Linux) */
#endif
    curl_off_t m_fileLen;           /**< Remote file size (bytes) */
    int64_t m_resume_len;           /**< Locally existing bytes (resume base) */
    int m_thread_num;               /**< Active chunk thread count */
    bool m_range_supported = false; /**< Whether Range is supported */
    int m_timeout;                  /**< Low-speed timeout seconds */
    string m_referer;               /**< Anti-hotlink Referer */
    string m_cookie;                /**< Request Cookie */
    std::atomic<bool> m_cancel_flag{false};  /**< Cancellation flag */
    std::string m_last_error;       /**< Most recent failure reason */
    bool m_range_known = false;     /**< Range confirmed via HEAD/
                                     *  Accept-Ranges */
    /* ---- P2: strict 206 + integrity ---- */
    std::atomic<bool> m_range_denied{false}; /**< Server answered 200 to
                                             *  Range */
    std::string m_remote_etag;              /**< Remote ETag from the probe */
    std::string m_remote_last_modified;     /**< Remote Last-Modified */

    /* ---- Chunk-level resume metadata (.curlbolt.part) ---- */
    std::vector<int64_t> m_part_written; /**< Written bytes per chunk */
    TMetaResult LoadPartMeta();          /**< Read meta; checks integrity */
    void SavePartMeta();                 /**< Write current chunk progress */
    void ClearPartMeta();                /**< Delete the meta file */
    std::string MetaPath() const;        /**< <target>.curlbolt.part */
};
