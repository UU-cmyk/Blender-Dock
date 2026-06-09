#pragma once

#include <map>
#include <string>
#include <vector>

class VersionDiscovery {
public:
	// 从 rootIndexUrl 获取主版本并在 totalBudgetMs 时间预算内并行发现每个主版本的下载
	std::map<std::string, std::vector<std::string>> discover(const std::string& rootIndexUrl, int totalBudgetMs);
};
