#include "download_utils.h"
#include <curl/curl.h>
#include <fstream>
#include <iostream>

// 写文件回调
static size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	std::ostream* os = static_cast<std::ostream*>(userdata);
	std::streamsize len = static_cast<std::streamsize>(size) * static_cast<std::streamsize>(nmemb);
	os->write(static_cast<char*>(ptr), len);
	return os->good() ? static_cast<size_t>(len) : 0;
}

// 进度回调（libcurl 7.32.0+ 使用 xferinfo）
static int progress_func(void* /*clientp*/, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
	if (dltotal > 0)
	{
		int pct = static_cast<int>((dlnow * 100) / dltotal);
		std::cout << "\rDownloading: " << pct << "% (" << dlnow << "/" << dltotal << " bytes)   " << std::flush;
	}
	return 0; // 返回非0将中止
}

bool download_file(const std::string& url, const std::filesystem::path& out_path, long timeout_ms)
{
	std::ofstream ofs(out_path, std::ios::binary);
	if (!ofs)
	{
		std::cerr << "Failed to open " << out_path.string() << " for writing\n";
		return false;
	}

	CURL* c = curl_easy_init();
	if (!c) return false;

	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteFileCallback);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &ofs);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	if (timeout_ms > 0) curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, nullptr);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_func);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);

	CURLcode r = curl_easy_perform(c);
	std::cout << std::endl;
	curl_easy_cleanup(c);
	ofs.close();
	return r == CURLE_OK;
}
