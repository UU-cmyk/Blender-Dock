#pragma once

#include <string>

// 下载 HTML 内容（成功返回 true）
bool download_html(const std::string& url, std::string& html, long timeout_ms = 15000);
