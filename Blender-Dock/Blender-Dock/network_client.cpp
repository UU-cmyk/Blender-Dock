#include "network_client.h"
#include <curl/curl.h>
#include <string>

// 写入回调：将接收到的数据追加到字符串缓冲区
static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, std::string* out)
{
	out->append(static_cast<char*>(ptr), size * nmemb);
	return size * nmemb;
}

// 下载 HTML 内容，超时以毫秒为单位
bool download_html(const std::string& url, std::string& html, long timeout_ms)
{
	CURL* curl = curl_easy_init();
	if (!curl) return false;
	html.clear();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return res == CURLE_OK;
}
