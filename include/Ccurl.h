/**
 * @file Ccurl.h
 * @brief 基于 libcurl 的多线程分片下载器 C++ 封装类声明（跨平台：Windows / Linux x86_64 / Linux aarch64）
 *
 * @author ErnestAgel
 * @date 2026-08-06
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

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace std;

/** @brief 最大分片下载线程数（-t 参数上限） */
#define MaxThread   10

/**
 * @brief 单个下载分片的任务信息结构体
 */
typedef struct 
{
    const char* url;       /**< 分片所属的下载 URL */
    char *file_ptr;        /**< 映射文件内存起始地址（mmap / MapViewOfFile） */
    int64_t offset;        /**< 本分片的起始字节偏移 */
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
     * @param thread_num 下载线程数（1 ~ MaxThread，默认 MaxThread；服务器不支持 Range 时自动退化为 1）
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
    bool m_range_supported;         /**< 服务器是否支持 HTTP Range */
    int m_timeout;                  /**< 低速超时秒数（0=不自动中断） */
    string m_referer;               /**< 防盗链 Referer（可为空） */
    string m_cookie;                /**< 请求 Cookie（可为空） */
};
