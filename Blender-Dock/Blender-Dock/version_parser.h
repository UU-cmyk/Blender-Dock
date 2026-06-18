#pragma once

#include <string>
#include <vector>
#include <set>

// 从字符串中解析版本号，例如 "Blender3.6.1" / "3.6.1" -> {3,6,1}
std::vector<int> parse_version_numbers(const std::string& s);

// 按嵌入的版本号比较文件名 (例如 "blender-4.2.10-...")
bool filename_version_less(const std::string& a, const std::string& b);

class VersionParser {
public:
	VersionParser() = default;

	// 从 HTML 解析主要版本目录名称（例如 "Blender3.6/"）
	std::vector<std::string> parseMajorVersions(const std::string& html) const;

	// 从 HTML 提取下载链接文件名（例如 "blender-3.6.1-windows-x64.zip"）
	std::set<std::string> parseDownloadLinks(const std::string& html) const;

	// 根据当前平台/架构过滤文件名集合并返回向量
	std::vector<std::string> filterBySystem(const std::set<std::string>& files) const;
};
