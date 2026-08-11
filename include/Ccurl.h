/**
 * @file Ccurl.h
 * @brief 基于 libcurl 的多线程分片下载器 C++ 封装类声明（跨平台：Windows / Linux x86_64 / Linux aarch64）
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

#include<iostream>
#include<curl/curl.h>
#include<string>
#include<mutex>
#include<vector>
#include<atomic>
#include<functional>
#include<ctime>
#include<thread>
#include<algorithm>

#include "../src/progress.h"

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace std;

/** @brief 最大分片下载线程数（-t 参数上限） */
/* 编译期硬上限（分片任务表数组大小）；实际可用上限随 CPU 核数自适应（BurstMaxThreads，不超过 8） */
#define MaxThread   8

/* ---- 自适应线程策略（运行时按 CPU 核数） ----
 * 逻辑核数检测失败时回退 8；
 * 上限 = clamp(核数, 4, 8)：任何机器最多 8 条连接（IDM 默认档位，对 CDN/服务器礼貌，
 *       超过 8 收益递减且更易触发限流/封禁）；
 * 默认 = clamp(核数, 2, 4)：主流机器默认 4 条，1~2 核弱机用 2 条。 */
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

/**
 * @brief 单个下载分片的任务信息结构体
 */
typedef struct 
{
    const char* url;       /**< 分片所属的下载 URL */
    char *file_ptr;        /**< 映射文件内存起始地址（mmap / MapViewOfFile） */
    int64_t offset;        /**< 本分片的起始字节偏移（下载过程中随写入推进） */
    int64_t end;           /**< 本分片的结束字节偏移 */
#ifdef _WIN32
    HANDLE thid;           /**< 下载线程句柄（Windows） */
#else
    pthread_t thid;        /**< 下载线程 ID（Linux） */
#endif
    double download_len;   /**< 本分片已下载的字节数 */
    bool use_range;        /**< 服务器是否支持 HTTP Range（false 时整段下载，不设 Range 头） */
    bool success;          /**< 本分片是否下载成功 */
    bool thread_created;   /**< 分片线程是否成功创建 */
    long timeout;          /**< 低速超时秒数（0=不限制，不自动中断） */
    const char* referer;   /**< 防盗链 Referer（可为空），如 B站视频流需 https://www.bilibili.com */
    const char* cookie;    /**< Cookie 字符串（可为空），部分流/文件需登录态 */
    /* ---- GUI 进度/取消扩展（Phase 1，见 gui-design.md §4.2/§5.2） ---- */
    int64_t part_start;    /**< 本分片初始起始偏移（File_Init 时保存，供计算分片总长与百分比） */
    int64_t part_total;    /**< 本分片总长（end - start + 1，回调里直接取用） */
    double  last_len;      /**< 上次进度回调时的下载量（算本线程速率） */
    time_t  last_t;        /**< 上次进度回调时间（算本线程速率） */
    std::atomic<bool>* cancel_flag;  /**< 指向所属 Ccurl 的取消标志（写回调/进度回调检查点） */
    const std::function<void(const std::vector<ThreadProgress>&,
                             double, double)>* on_progress;  /**< 指向 Ccurl::onProgress（可空） */
}st_EasyList;


/**
 * @brief 多线程分片下载器（跨平台）
 */
class Ccurl
{

public:
    /**
     * @brief 构造函数：初始化分片任务表并打印 libcurl 版本信息
     */
    Ccurl();

    /**
     * @brief 析构函数，释放分片任务与内存映射
     */
    ~Ccurl();

    /**
     * @brief 初始化下载任务：保存 URL 与文件名，检测 Range 支持并创建本地文件
     * @param url 下载地址
     * @param filename 保存到本地的文件名
     * @param thread_num 下载线程数（1 ~ MaxThread；默认由调用方按核数自适应；服务器不支持 Range 时自动退化为 1）
     * @param timeout 低速超时秒数：下载无进展（低于 1 字节/秒）持续 timeout 秒则中断（默认 60；0 表示不自动中断）
     * @return 初始化是否成功
     */
    bool Init(const string url, string filename, int thread_num = MaxThread, int timeout = 60);
    
    /**
     * @brief 启动多线程分片下载：创建并等待所有下载线程
     * @return 所有分片是否全部成功
     */
    bool Download_Task();

    /**
     * @brief 上传任务（预留接口）
     * @param server_url 服务器地址
     * @return 是否成功
     */
    bool Uploading_Task(const char* server_url);

    /**
     * @brief 设置防盗链 Referer（部分视频流/资源需要，如 B站）
     * @param referer 来源页 URL，可为空字符串
     */
    void SetReferer(const string& referer);

    /**
     * @brief 设置请求 Cookie（部分视频流/资源需要登录态，如 B站高清流）
     * @param cookie Cookie 字符串（如 "SESSDATA=xxx; bili_jct=xxx"），可为空字符串
     */
    void SetCookie(const string& cookie);

    /**
     * @brief 请求取消：置取消标志，下载线程在写回调/进度回调检查点中止（延迟 < 1s）
     * @note GUI 取消按钮与关窗退出调用；取消后残留分片文件保留（供断点续传）
     */
    void Cancel();

    /**
     * @brief 下载初始化完成后、开始传输前：返回各分片信息（0 进度占位）
     * @return 每分片 ThreadProgress（downloaded=分片起点，未开始下载）
     * @note UI 据此在首个进度回调前就绘制"电池格"分隔线
     */
    std::vector<ThreadProgress> SnapshotParts() const;

    /**
     * @brief 查询是否已请求取消
     * @return 已取消返回 true
     */
    bool IsCanceled() const;

    /**
     * @brief 最近一次失败的具体原因（Init/Download_Task 失败后读取，供 GUI 弹窗显示）
     * @return 错误描述（UTF-8；无错误时为空串）
     */
    std::string LastError() const;

    /**
     * @brief 进度回调（GUI 注入）：每个节流周期（~200ms）调用一次
     * @note 参数：(各分片进度, 总百分比, 总速率 B/s)；CLI 不设置则保留原 1% 门控打印
     */
    std::function<void(const std::vector<ThreadProgress>&, double, double)> onProgress;

    /**
     * @brief 线程入口：下载单个分片
     * @param arg 指向 st_EasyList 的指针
     * @return 线程返回值（恒为 nullptr）
     */
    static void* Downloading(void* arg);
    
    st_EasyList *m_Easy_List[MaxThread + 1];   /**< 分片任务表 */


private:

    /**
     * @brief 通过 HEAD 请求探测文件总大小
     * @return 探测是否成功
     */
    bool get_Download_FileSize();

    /**
     * @brief 检测服务器是否支持 HTTP Range（GET + Range: 0-0，检查是否返回 206）
     * @return 支持返回 true，否则 false
     */
    bool Check_Range_Support();

    /**
     * @brief 初始化本地文件：检测断点、创建文件、内存映射并按线程数均分区间
     * @param filename 本地文件名
     * @return 初始化是否成功
     */
    bool File_Init(const char* filename);

    /**
     * @brief 释放分片任务、解除内存映射并关闭文件
     */
    void Destory();

    void* m_easyHandle = nullptr;   /**< libcurl easy handle */
    string m_filename;              /**< 本地文件名 */
    string m_url;                   /**< 下载地址 */
    std::mutex m_lock;              /**< 互斥锁 */
    char* m_pTrunck = nullptr;      /**< 内存映射地址（mmap / MapViewOfFile） */
#ifdef _WIN32
    HANDLE m_hFile = INVALID_HANDLE_VALUE;   /**< 文件句柄（Windows） */
    HANDLE m_hMapping = nullptr;             /**< 文件映射句柄（Windows） */
#else
    int m_fd = -1;                  /**< 本地文件描述符（Linux） */
#endif
    curl_off_t m_fileLen;           /**< 远程文件总大小（字节） */
    int64_t m_resume_len;           /**< 本地已存在字节数（断点续传起点） */
    int m_thread_num;               /**< 实际使用的下载线程数 */
    bool m_range_supported = false; /**< 服务器是否支持 HTTP Range */
    int m_timeout;                  /**< 低速超时秒数（0=不自动中断） */
    string m_referer;               /**< 防盗链 Referer（可为空） */
    string m_cookie;                /**< 请求 Cookie（可为空） */
    std::atomic<bool> m_cancel_flag{false};  /**< 取消标志（Cancel() 置位，回调检查点读取） */
    std::string m_last_error;       /**< 最近一次失败的具体原因（LastError() 读取） */
    bool m_range_known = false;     /**< Range 支持是否已由 HEAD/Accept-Ranges 确认（省一次探测请求） */

    /* ---- 分片级断点续传元数据（.curlbolt.part，修复 mmap 预分配导致
     *      stat 文件大小恒等于 m_fileLen、resume 误判"已完整"的问题） ---- */
    std::vector<int64_t> m_part_written; /**< 每分片实际已写入字节数（顺序对应分片序） */
    bool LoadPartMeta();                 /**< 读元数据（校验 filelen 匹配）→ m_part_written */
    void SavePartMeta();                 /**< 写当前各分片已写字节 */
    void ClearPartMeta();                /**< 删除元数据文件 */
    std::string MetaPath() const;        /**< <目标文件>.curlbolt.part */
};
