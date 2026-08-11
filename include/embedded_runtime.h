#ifndef EMBEDDED_RUNTIME_H
#define EMBEDDED_RUNTIME_H

#include <string>

/**
 * @brief 记录可执行文件路径（argv[0]），供运行时定位使用
 */
void EmbedSetExePath(const std::string& exe_path);

/**
 * @brief 获取已记录的可执行文件路径
 */
std::string EmbedGetExePath();

/**
 * @brief 确保运行时资源就绪，输出其根目录
 * @param home 输出：就绪后的运行时根目录（含 stdlib/）；不可用时为空串
 * @return 是否成功（不可用时返回 false，调用方继续回退链）
 * @note 缓存存在且版本标记匹配时直接复用；被清理后自动重建
 */
bool ExtractEmbeddedRuntime(std::string& home);

#endif  // EMBEDDED_RUNTIME_H
