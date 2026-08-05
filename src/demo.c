/**
 * @file demo.c
 * @brief 基于 libcurl 的多线程分片下载器 C 语言实现
 *
 * @author Ernest
 * @date 2026-08-06
 */

#include <stdio.h>
#include <unistd.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/**
 * @brief 单个下载分片的任务信息结构体
 */
struct fileInfo {
	const char *url;        /**< 分片所属的下载 URL */
	char *fileptr;          /**< mmap 映射的文件内存起始地址 */
	int offset;             /**< 本分片的起始字节偏移 */
	int end;                /**< 本分片的结束字节偏移 */
	pthread_t thid;         /**< 下载线程 ID */
	double download;        /**< 本分片已下载的字节数 */
};

/** @brief 分片下载线程数（实际创建 THREAD_NUM + 1 个线程，最后一个负责余数部分） */
#define THREAD_NUM		10


struct fileInfo **pInfoTable;   /**< 全局分片任务表指针，供进度回调汇总各线程下载量 */
double downloadFileLength = 0;  /**< 待下载文件总大小（字节） */

/**
 * @brief libcurl 写回调：将下载数据写入 mmap 映射内存的对应偏移
 * @param ptr 收到的数据指针
 * @param size 单个数据块大小
 * @param memb 数据块数量
 * @param userdata 指向 struct fileInfo 的用户数据
 * @return 实际写入的字节数
 */
size_t writeFunc(void *ptr, size_t size, size_t memb, void *userdata) {
	struct fileInfo *info = (struct fileInfo *)userdata;

	memcpy(info->fileptr + info->offset, ptr, size * memb);
	info->offset += size * memb;
	

	return size * memb;
}

/**
 * @brief libcurl 进度回调：汇总各线程下载量并打印整体百分比
 * @param userdata 指向 struct fileInfo 的用户数据
 * @param totalDownload 总下载字节数
 * @param nowDownload 当前已下载字节数
 * @param totalUpload 总上传字节数
 * @param nowUpload 当前已上传字节数
 * @return 0 表示继续下载
 */
int progressFunc(void *userdata, double totalDownload, double nowDownload, 
				double totalUpload, double nowUpload) {

	int percent = 0;
	static int print = 1;
	struct fileInfo *info = (struct fileInfo*)userdata;
	info->download = nowDownload;
	
	if (totalDownload > 0) {

		int i = 0;
		double allDownload = 0;
		for (i = 0;i <= THREAD_NUM;i ++) {
			allDownload += pInfoTable[i]->download;
		}
		
		percent = (int)(allDownload / downloadFileLength * 100);
	}

	if (percent == print) {
		printf("thid:%ld percent: %d%%\n",info->thid, percent);
		print += 1;
	}

	return 0;
}

/**
 * @brief 通过 HEAD 请求探测文件总大小
 * @param url 下载地址
 * @return 文件总大小（失败时返回 -1）
 */
double getDownloadFileLength(const char *url) {

	CURL *curl = curl_easy_init();

	printf("url: %s\n", url);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36");
	curl_easy_setopt(curl, CURLOPT_HEADER, 1);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
	
	CURLcode res = curl_easy_perform(curl);
	if (res == CURLE_OK) {
		printf("downloadFileLength success\n");
		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &downloadFileLength);
	} else {
		printf("downloadFileLength error\n");
		downloadFileLength = -1;
	}
	curl_easy_cleanup(curl);

	return downloadFileLength;
}


/**
 * @brief 线程入口：下载一个分片（Range: offset-end）
 * @param arg 指向 struct fileInfo 的指针
 * @return 线程返回值（恒为 NULL）
 */
void *worker(void *arg) {

	struct fileInfo *info = (struct fileInfo*)arg;

	char range[64] = {0};
	snprintf(range, 64, "%d-%d", info->offset, info->end);

	printf("threadid: %ld, download from: %d to: %d\n", info->thid, info->offset, info->end);
	CURL *curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_URL, info->url);
	
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, info); 
	
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressFunc);
	curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, info);
	
	curl_easy_setopt(curl, CURLOPT_RANGE, range);
	
	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		printf("res %d\n", res);
	}
	curl_easy_cleanup(curl);

	return NULL;
}


/**
 * @brief 多线程分片下载主流程：探测大小、创建文件、mmap 映射、分片并等待所有线程
 * @param url 下载地址
 * @param filename 保存到本地的文件名
 * @return 0 表示成功，-1 表示失败
 */
int download(const char *url, const char *filename) {

	long fileLength = getDownloadFileLength(url);
	printf("downloadFileLength: %ld\n", fileLength);
	

	int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		return -1;
	}

	if (-1 == lseek(fd, fileLength-1, SEEK_SET)) {
		perror("lseek");
		close(fd);
		return -1;
	}
	if (1 != write(fd, "", 1)) {
		perror("write");
		close(fd);
		return -1;
	}

	char *fileptr = (char *)mmap(NULL, fileLength, PROT_READ| PROT_WRITE, MAP_SHARED, fd, 0);
	if (fileptr == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return -1;
	}
	/**
	 * 分片示意：文件被均分为 THREAD_NUM 段，最后一个线程负责余数部分：
	 * 0 ~ partSize-1, partSize ~ 2*partSize-1, ..., 最后一段到 fileLength-1
	 */
	int i = 0;
	long partSize = fileLength / THREAD_NUM;
	struct fileInfo *info[THREAD_NUM+1] = {NULL};
	
	for (i = 0;i <= THREAD_NUM;i ++) {

		info[i] = (struct fileInfo*)malloc(sizeof(struct fileInfo));
		
		info[i]->offset = i * partSize;
		if (i < THREAD_NUM) {
			info[i]->end = (i+1) * partSize - 1;
		} else {
			info[i]->end = fileLength - 1;
		}
		info[i]->fileptr = fileptr;
		info[i]->url = url;
		info[i]->download = 0;
	}
	pInfoTable = info;

	for (i = 0;i <= THREAD_NUM;i ++) {
		pthread_create(&(info[i]->thid), NULL, worker, info[i]);
	}

	for (i = 0;i <= THREAD_NUM;i ++) {
		pthread_join(info[i]->thid, NULL);
	}
	
	for (i = 0;i <= THREAD_NUM;i ++) {		
		free(info[i]);
	}
	munmap(fileptr, fileLength);
	close(fd);
	
	
	return 0;
}

/**
 * @brief 程序入口
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 程序退出码
 */
int main(int argc, const char *argv[]) {
	

	return download("https://releases.ubuntu.com/20.04/ubuntu-20.04.6-live-server-amd64.iso.zsync", "ubuntu.zsync");

}
