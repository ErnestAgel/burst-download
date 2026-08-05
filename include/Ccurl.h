/**
 * @file Ccurl.h
 * @brief 基于 libcurl 的多线程分片下载器 C++ 封装类声明
 *
 * @author ErnestAgel
 * @date 2026-08-06
 */

#include<iostream>
#include<curl/curl.h>
#include<string>
#include<mutex>
#include<vector>

using namespace std;

/** @brief 分片下载线程数（实际创建 MaxThread + 1 个线程，最后一个负责余数部分） */
#define MaxThread   10


typedef struct 
{
    const char* url;       /**< 分片所属的下载 URL */
    char *file_ptr;        /**< mmap 映射的文件内存起始地址 */
    int32_t offset;        /**< 本分片的起始字节偏移 */
    int32_t end;           /**< 本分片的结束字节偏移 */
    pthread_t thid;        /**< 下载线程 ID */
    double download_len;   /**< 本分片已下载的字节数 */
}st_EasyList;


/**
 * @brief 多线程分片下载器
 */
class Ccurl
{

public:
    /**
     * @brief 构造函数，打印 libcurl 版本信息
     */
    Ccurl();

    /**
     * @brief 析构函数，释放分片任务与内存映射
     */
    ~Ccurl();

    /**
     * @brief 初始化下载任务：保存 URL 与文件名，并创建本地文件
     * @param url 下载地址
     * @param filename 保存到本地的文件名
     * @return 初始化是否成功
     */
    bool Init(const string url, string filename);
    
    /**
     * @brief 启动多线程分片下载：创建并等待所有下载线程
     * @return 是否启动成功
     */
    bool Download_Task();

    /**
     * @brief 上传任务（预留接口）
     * @param server_url 服务器地址
     * @return 是否成功
     */
    bool Uploading_Task(const char* server_url);

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
     * @brief 初始化本地文件：创建文件、mmap 映射并按 MaxThread 均分区间
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
    char* m_pTrunck = nullptr;      /**< mmap 映射的内存地址 */
    int m_fd;                       /**< 本地文件描述符 */
    vector<std::thread> m_threads;  /**< 下载线程列表（当前使用 pthread） */
    double m_fileLen;               /**< 文件总大小（字节） */
};
