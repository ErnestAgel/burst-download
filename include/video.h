/**
 * @file video.h
 * @brief 视频直链解析模块：通过外部 yt-dlp 解析视频网页 URL，得到可直接下载的媒体流地址
 *
 * @author ErnestAgel
 * @date 2026-08-06
 */

#include <string>
#include <vector>

/**
 * @brief 通过 yt-dlp 解析视频网页 URL，得到可直接下载的媒体流地址列表
 * @param url 视频网页 URL（如 B站/YouTube 等视频页，yt-dlp 支持 1000+ 网站）
 * @param urls 输出：媒体流 URL 列表（DASH 分离时依次为视频轨、音频轨）
 * @param cookies_from_browser 从浏览器读取登录 Cookie（chrome/firefox/edge 等，可为空）：
 *        用于解析需要登录态的高清视频流（如 B站 720p+）
 * @param cookies_file Netscape 格式 Cookie 文件路径（可为空），与 cookies_from_browser 二选一
 * @return 解析是否成功（失败时 urls 为空）
 * @note 依赖系统已安装 yt-dlp（pip install yt-dlp 或官网单文件），支持网站与 yt-dlp 一致
 */
bool ParseVideoUrls(const std::string& url, std::vector<std::string>& urls,
                    const std::string& cookies_from_browser = "",
                    const std::string& cookies_file = "");
