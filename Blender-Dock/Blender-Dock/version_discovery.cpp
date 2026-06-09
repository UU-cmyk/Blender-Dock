#include "version_discovery.h"
#include "version_parser.h"
#include "network_client.h"
#include <future>
#include <chrono>
#include <iostream>

std::map<std::string, std::vector<std::string>> VersionDiscovery::discover(const std::string& rootIndexUrl, int totalBudgetMs)
{
	std::map<std::string, std::vector<std::string>> available_versions;

	std::string html;
	if (!download_html(rootIndexUrl, html, 8000)) {
		std::cerr << "下载主索引失败: " << rootIndexUrl << "\n";
		return available_versions;
	}

	VersionParser vp;
	auto majors = vp.parseMajorVersions(html);

	const auto start_time = std::chrono::steady_clock::now();
	const std::chrono::milliseconds budget(totalBudgetMs);

	std::vector<std::future<std::pair<std::string, std::vector<std::string>>>> futures;
	for (const auto& ver : majors)
	{
		futures.emplace_back(std::async(std::launch::async, [ver, start_time, budget]() -> std::pair<std::string, std::vector<std::string>> {
			std::pair<std::string, std::vector<std::string>> result;
			result.first = ver;

			auto now = std::chrono::steady_clock::now();
			long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_time + budget - now).count();
			if (remaining_ms <= 0) return result;

			std::string url = std::string("https://download.blender.org/release/") + ver + "/";
			std::string sub_html;
			long timeout = static_cast<long>(std::min<long long>(remaining_ms, 3000LL));
			if (!download_html(url, sub_html, timeout)) return result;

			VersionParser vp_local;
			auto files_set = vp_local.parseDownloadLinks(sub_html);
			auto system_files = vp_local.filterBySystem(files_set);
			result.second = std::move(system_files);
			return result;
		}));
	}

	for (auto& f : futures)
	{
		auto now = std::chrono::steady_clock::now();
		long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_time + budget - now).count();
		if (remaining_ms <= 0) { std::cerr << "Time budget exceeded while gathering links\n"; break; }
		if (f.wait_for(std::chrono::milliseconds(remaining_ms)) == std::future_status::ready)
		{
			auto p = f.get();
			if (!p.second.empty()) available_versions[p.first] = p.second;
		}
		else
		{
			std::cerr << "某个版本的抓取超时\n";
		}
	}

	return available_versions;
}
