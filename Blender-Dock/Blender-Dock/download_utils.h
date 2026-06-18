#pragma once
#include <string>
#include <filesystem>

// 下载文件到指定路径（覆盖），timeout_ms=0 表示无超时
bool download_file(const std::string& url, const std::filesystem::path& out_path, long timeout_ms = 0);
