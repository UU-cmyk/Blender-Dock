#include "version_discovery.h"
#include "version_parser.h"
#include "network_client.h"
#include <future>
#include <chrono>
#include <thread>
#include <memory>
#include <iostream>

namespace {
	constexpr int MAX_CONCURRENT = 4;
	constexpr const char* RELEASE_BASE_URL = "https://download.blender.org/release/";
}

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

	// 分批启动，每批最多 MAX_CONCURRENT 个并发下载
	for (size_t batch_start = 0; batch_start < majors.size(); )
	{
		const size_t batch_end = (std::min)(batch_start + MAX_CONCURRENT, majors.size());
		std::vector<std::future<std::pair<std::string, std::vector<std::string>>>> futures;
		futures.reserve(batch_end - batch_start);

		for (size_t i = batch_start; i < batch_end; ++i)
		{
			auto now = std::chrono::steady_clock::now();
			long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_time + budget - now).count();
			if (remaining_ms <= 0)
			{
				std::cerr << "Time budget exceeded before launching tasks\n";
				return available_versions;
			}

			// init-capture by move 避免拷贝 major 字符串
			std::string major_ver = majors[i];
			futures.emplace_back(std::async(std::launch::async,
				[ver = std::move(major_ver), start_time, budget]()
					-> std::pair<std::string, std::vector<std::string>>
			{
				std::pair<std::string, std::vector<std::string>> result;

				auto now = std::chrono::steady_clock::now();
				long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					start_time + budget - now).count();
				if (remaining_ms <= 0) return result;

				// 构建 URL：用 += 替代 + 拼接，减少临时字符串分配
				std::string url;
				url.reserve(64);
				url += RELEASE_BASE_URL;
				url += ver;
				url += '/';

				std::string sub_html;
				long timeout = static_cast<long>((std::min)(remaining_ms, 3000LL));
				if (!download_html(url, sub_html, timeout)) return result;

				VersionParser vp_local;
				auto files_set = vp_local.parseDownloadLinks(sub_html);
				auto system_files = vp_local.filterBySystem(files_set);
				result.first = std::move(ver);
				result.second = std::move(system_files);
				return result;
			}));
		}

		// 等待本批次全部完成
		auto abandoned = std::make_shared<std::vector<std::future<std::pair<std::string, std::vector<std::string>>>>>();
		for (auto& f : futures)
		{
			auto now = std::chrono::steady_clock::now();
			long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				start_time + budget - now).count();
			if (remaining_ms <= 0)
			{
				std::cerr << "Time budget exceeded while gathering links\n";
				// 将全部 future（含当前）转移到后台线程，避免析构阻塞
				for (auto it = futures.begin(); it != futures.end(); ++it)
				{
					abandoned->push_back(std::move(*it));
				}
				std::thread([abandoned]() mutable {
					for (auto& rf : *abandoned) rf.wait();
				}).detach();
				return available_versions;
			}

			if (f.wait_for(std::chrono::milliseconds(remaining_ms)) == std::future_status::ready)
			{
				auto p = f.get();
				if (!p.second.empty())
					available_versions[std::move(p.first)] = std::move(p.second);
			}
			else
			{
				std::cerr << "某个版本的抓取超时\n";
				abandoned->push_back(std::move(f));
			}
		}
		if (!abandoned->empty())
		{
			std::thread([abandoned]() mutable {
				for (auto& rf : *abandoned) rf.wait();
			}).detach();
		}

		batch_start = batch_end;
	}

	return available_versions;
}
