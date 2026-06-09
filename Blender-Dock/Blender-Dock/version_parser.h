#pragma once

#include <string>
#include <vector>
#include <set>

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
