#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <curl/curl.h>
#include <gumbo.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fstream>
#include <filesystem>
#include <future>
#include <atomic>
#include <chrono>
#include <boost/dll.hpp>
#include <boost/program_options.hpp>
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86) || defined(__i386__)
#include <cpu_features/cpuinfo_x86.h>
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
#include <cpu_features/cpuinfo_arm.h>
#elif defined(__powerpc__) || defined(__ppc__)
#include <cpu_features/cpuinfo_powerpc.h>
#else
// unknown architecture: do not include specific cpu_features headers
#endif
#include <boost/predef/os.h>
#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#include <sys/utsname.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <zip.h>

/// <summary>
/// 数据写入回调函数，该函数在 libcurl 接收到 HTTP 响应数据时被调用，用于将接收到的数据追加到用户提供的 std::string 缓冲区中
/// </summary>
/// <param name="ptr">libcurl 收到的数据缓冲区</param>
/// <param name="size">每个数据块的大小</param>
/// <param name="nmemb">数据块的数量</param>
/// <param name="out">传入的自定义参数</param>
/// <returns></returns>
static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, std::string* out)
{
	out->append(static_cast<char*>(ptr), size * nmemb);
	return size * nmemb;
}
// Forward declare parse_version_numbers used below
static std::vector<int> parse_version_numbers(const std::string& s);

// 按嵌入的版本号比较文件名 (例如 "blender-4.2.10-...")
static bool filename_version_less(const std::string& a, const std::string& b)
{
	auto va = parse_version_numbers(a);
	auto vb = parse_version_numbers(b);
	size_t n = (va.size() > vb.size()) ? va.size() : vb.size();
	for (size_t i = 0; i < n; ++i) {
		int ai = i < va.size() ? va[i] : 0;
		int bi = i < vb.size() ? vb[i] : 0;
		if (ai < bi) return true;
		if (ai > bi) return false;
	}
	return a < b;
}

// 从字符串中解析版本号，例如 "Blender3.6.1" / "3.6.1" -> {3,6,1}
static std::vector<int> parse_version_numbers(const std::string& s)
{
	std::vector<int> parts;
	size_t pos = s.find_first_of("0123456789");
	if (pos == std::string::npos) return parts;
	std::string sub = s.substr(pos);
	std::string cur;
	for (char c : sub) {
		if (std::isdigit(static_cast<unsigned char>(c))) {
			cur.push_back(c);
		}
		else if (c == '.') {
			if (!cur.empty()) { parts.push_back(std::stoi(cur)); cur.clear(); }
			else parts.push_back(0);
		}
		else {
			break; // stop at first non-digit/dot
		}
	}
	if (!cur.empty()) parts.push_back(std::stoi(cur));
	return parts;
}

// Compare version strings by numeric parts (ascending). If equal, fallback to string compare.
static bool version_less_numeric(const std::string& a, const std::string& b)
{
	auto va = parse_version_numbers(a);
	auto vb = parse_version_numbers(b);
	size_t n = (va.size() > vb.size()) ? va.size() : vb.size();
	for (size_t i = 0; i < n; ++i) {
		int ai = i < va.size() ? va[i] : 0;
		int bi = i < vb.size() ? vb[i] : 0;
		if (ai < bi) return true;
		if (ai > bi) return false;
	}
	return a < b;
}

// 从文件加载配置（简单密钥格式返回下载基目录
// 简单的 JSON 配置加载器（无外部 JSON 依赖项）
struct SimpleConfig {
	std::filesystem::path download_dir;
	unsigned int parallel = 0; // 0 表示自动（使用硬件并发）

	std::filesystem::path addons_dir = "addons";
	std::filesystem::path assets_dir = "assets";
	bool auto_extract_addons = true;
	bool auto_link_addons = false;  // 是否创建符号链接到Blender用户目录
	std::string blender_user_dir = ""; // 自定义Blender用户目录路径
};

static std::string read_file_all(const std::filesystem::path& p)
{
	std::ifstream ifs(p);
	if (!ifs) return std::string();
	std::ostringstream ss;
	ss << ifs.rdbuf();
	return ss.str();
}

static void write_default_config(const std::filesystem::path& cfgpath)
{
	try {
		std::filesystem::create_directories(cfgpath.parent_path());
		std::ofstream ofs(cfgpath, std::ios::trunc);
		if (!ofs) return;
		ofs << "{\n";
		ofs << "  \"download_dir\": \"versions\",\n";
		ofs << "  \"parallel_downloads\": 0\n";
		ofs << "}\n";
	}
	catch (...) {}
}

static SimpleConfig load_config_json(const std::string& cfgfile = "config/config.json")
{
	SimpleConfig cfg;
	cfg.download_dir = std::filesystem::path("versions");
	cfg.addons_dir = std::filesystem::path("addons");
	cfg.assets_dir = std::filesystem::path("assets");
	cfg.parallel = 0;
	cfg.auto_extract_addons = true;
	cfg.auto_link_addons = false;

	std::filesystem::path p(cfgfile);
	if (!std::filesystem::exists(p)) {
		write_default_config(p);
		return cfg;
	}

	std::string s = read_file_all(p);
	if (s.empty()) return cfg;

	// 粗略的 JSON 解析查找 "download_dir"
	auto find_key = [&](const std::string& key) -> std::string {
		size_t pos = s.find('"' + key + '"');
		if (pos == std::string::npos) return std::string();
		pos = s.find(':', pos);
		if (pos == std::string::npos) return std::string();
		pos++;
		// skip spaces
		while (pos < s.size() && isspace(static_cast<unsigned char>(s[pos]))) ++pos;
		if (pos >= s.size()) return std::string();
		if (s[pos] == '"') {
			++pos;
			size_t end = pos;
			while (end < s.size() && s[end] != '"') ++end;
			if (end <= s.size()) return s.substr(pos, end - pos);
			return std::string();
		}
		else {
			// number or literal
			size_t end = pos;
			while (end < s.size() && !isspace(static_cast<unsigned char>(s[end])) && s[end] != ',' && s[end] != '}') ++end;
			return s.substr(pos, end - pos);
		}
		};

	auto addons = find_key("addons_dir");
	if (!addons.empty()) cfg.addons_dir = std::filesystem::path(addons);

	auto assets = find_key("assets_dir");
	if (!assets.empty()) cfg.assets_dir = std::filesystem::path(assets);

	auto auto_extract = find_key("auto_extract_addons");
	if (!auto_extract.empty()) cfg.auto_extract_addons = (auto_extract == "true");

	auto auto_link = find_key("auto_link_addons");
	if (!auto_link.empty()) cfg.auto_link_addons = (auto_link == "true");

	auto dld = find_key("download_dir");
	if (!dld.empty()) cfg.download_dir = std::filesystem::path(dld);

	auto par = find_key("parallel_downloads");
	if (!par.empty()) {
		try { cfg.parallel = static_cast<unsigned int>(std::stoul(par)); }
		catch (...) { cfg.parallel = 0; }
	}
	return cfg;
}

// 确保工作区文件夹结构存在
static void ensure_workspace_structure()
{
	try {
		std::filesystem::create_directories("bin");
		std::filesystem::create_directories("config");
		std::filesystem::create_directories("versions");
		std::filesystem::create_directories("addons");
		std::filesystem::create_directories("assets");
		std::filesystem::create_directories("logs");
	}
	catch (...) {}
}

// 尝试将 .zip 文件解压缩到目标目录成功时返回 true
static bool extract_zip(const std::filesystem::path& zipfile, const std::filesystem::path& dest)
{
	try {
		std::filesystem::create_directories(dest);
	}
	catch (...) {
		return false;
	}

	int err = 0;
	zip_t* za = zip_open(zipfile.string().c_str(), ZIP_RDONLY, &err);
	if (!za) {
		std::cerr << "libzip: failed to open archive: " << zipfile.string() << " (err=" << err << ")\n";
		return false;
	}

	// RAII guard to ensure zip_close is always called
	struct ZipCloser { zip_t* za; ZipCloser(zip_t* p) : za(p) {} ~ZipCloser() { if (za) zip_close(za); } } zguard(za);

	zip_int64_t n = zip_get_num_entries(za, 0);
	for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(n); ++i) {
		struct zip_stat st;
		if (zip_stat_index(za, i, 0, &st) != 0) continue;
		std::string name = st.name;
		// Normalize name separators
		std::filesystem::path entry_path = std::filesystem::path(name);

		std::filesystem::path outpath = dest / entry_path;

		// 目录条目（名称以“/”结尾）
		if (!name.empty() && (name.back() == '/' || name.back() == '\\')) {
			try { std::filesystem::create_directories(outpath); }
			catch (...) {}
			continue;
		}

		// 确保父目录存在
		if (outpath.has_parent_path()) {
			try { std::filesystem::create_directories(outpath.parent_path()); }
			catch (...) {}
		}

		zip_file_t* zf = zip_fopen_index(za, i, 0);
		if (!zf) {
			std::cerr << "libzip: failed to open file in archive: " << name << "\n";
			return false;
		}

		// RAII guard for opened file in archive
		struct ZipFileCloser { zip_file_t* zf; ZipFileCloser(zip_file_t* p) : zf(p) {} ~ZipFileCloser() { if (zf) zip_fclose(zf); } } zfguard(zf);

		std::ofstream ofs(outpath, std::ios::binary);
		if (!ofs) {
			std::cerr << "Failed to create output file: " << outpath.string() << "\n";
			return false;
		}

		const size_t BUF_SIZE = 8192;
		std::vector<char> buffer(BUF_SIZE);
		zip_int64_t bytesRead = 0;
		while ((bytesRead = zip_fread(zf, buffer.data(), static_cast<zip_uint64_t>(buffer.size()))) > 0) {
			ofs.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
			if (!ofs) {
				std::cerr << "Write failed to: " << outpath.string() << "\n";
				return false;
			}
		}

		// explicit close of ofstream handled by destructor
		ofs.close();
	}

	return true;
}

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

// 下载文件到指定路径（覆盖）
static bool download_file(const std::string& url, const std::filesystem::path& out_path, long timeout_ms = 0)
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

/// <summary>
/// 下载 HTML，允许传入超时毫秒数（默认 15000 ms）
/// </summary>
/// <param name="url">目标网页地址（支持 HTTP / HTTPS）</param>
/// <param name="html">用于接收响应内容的字符串</param>
/// <returns></returns>
static bool download_html(const std::string& url, std::string& html, long timeout_ms = 15000)
{
	CURL* curl = curl_easy_init();
	if (!curl) return false;

	html.clear();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	// 使用毫秒级超时以更精确控制
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	return res == CURLE_OK;
}

/// <summary>
/// 解析 Blender 大版本目录
/// </summary>
/// <param name="node">当前 Gumbo DOM 节点</param>
/// <param name="out">用于存储提取到的版本目录名的字符串数组</param>
static void parse_major_versions(GumboNode* node, std::vector<std::string>& out)
{
	if (node->type != GUMBO_NODE_ELEMENT) return;

	if (node->v.element.tag == GUMBO_TAG_A)
	{
		GumboAttribute* href =
			gumbo_get_attribute(&node->v.element.attributes, "href");

		if (href)
		{
			std::string s = href->value;
			if (s.find("Blender") == 0 && s.back() == '/')
			{
				s.pop_back();

				// 过滤不需要输出的目录名：BlenderBenchmark, alpha, beta, trailing 'a'/'b'/'c', newpy
				auto tolower_copy = [](const std::string& str) { std::string t; t.reserve(str.size()); for (char c : str) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); return t; };
				std::string low = tolower_copy(s);
				bool exclude = false;
				if (low.find("blenderbenchmark") != std::string::npos) exclude = true;
				if (low.find("alpha") != std::string::npos) exclude = true;
				if (low.find("beta") != std::string::npos) exclude = true;
				if (low.find("newpy") != std::string::npos) exclude = true;
				// 排除带后缀 a/b/c 的版本
				if (!exclude && !s.empty()) {
					char last = s.back();
					if ((last == 'a' || last == 'A' || last == 'b' || last == 'B' || last == 'c' || last == 'C') && s.size() >= 2 && std::isdigit(static_cast<unsigned char>(s[s.size() - 2]))) {
						exclude = true;
					}
				}

				if (!exclude)
					out.push_back(s);
			}
		}
	}

	auto* c = &node->v.element.children;
	for (unsigned i = 0; i < c->length; ++i)
		parse_major_versions(static_cast<GumboNode*>(c->data[i]), out);
}

/// <summary>
/// 解析子目录中的下载文件
/// </summary>
/// <param name="node">当前 Gumbo DOM 节点</param>
/// <param name="out">用于存储提取到的下载链接（自动去重）</param>
static void parse_download_links(GumboNode* node, std::set<std::string>& out)
{
	if (node->type != GUMBO_NODE_ELEMENT) return;

	if (node->v.element.tag == GUMBO_TAG_A)
	{
		GumboAttribute* href =
			gumbo_get_attribute(&node->v.element.attributes, "href");

		if (href)
		{
			std::string s = href->value;
			if (s.ends_with(".zip") ||
				s.ends_with(".tar.xz") ||
				s.ends_with(".dmg"))
			{
				out.insert(s);
			}
		}
	}

	auto* c = &node->v.element.children;
	for (unsigned i = 0; i < c->length; ++i)
		parse_download_links(static_cast<GumboNode*>(c->data[i]), out);
}

/// <summary>
/// 获取当前操作系统名称，使用 Boost.Predef 库进行检测，以便在输出系统信息时显示更友好的名称
/// </summary>
/// <returns>返回操作系统名称</returns>
static std::string get_system_name()
{
	if (BOOST_OS_WINDOWS) {
		return "Windows";
	}
	else if (BOOST_OS_LINUX) {
		return  "Linux";
	}
	else if (BOOST_OS_MACOS) {
		return "macOS";
	}
	else {
		return  "Unknown OS";
	}
}

/// <summary>
/// 根据当前操作系统过滤可用的Blender安装包
/// </summary>
/// <param name="files">所有文件集合</param>
/// <returns>返回过滤后的文件集合</returns>
static std::vector<std::string> filter_by_system(const std::set<std::string>& files)
{
	std::vector<std::string> available;

	// helper: get normalized architecture string: "x86_64", "x86", "arm64", "armv7", etc.
	auto get_arch = []() -> std::string {
#if BOOST_OS_WINDOWS
		SYSTEM_INFO si;
		GetNativeSystemInfo(&si);
		switch (si.wProcessorArchitecture) {
		case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
		case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
		case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
		case PROCESSOR_ARCHITECTURE_ARM: return "arm";
		default: return "unknown";
		}
#else
		struct utsname u;
		if (uname(&u) != 0) return "unknown";
		std::string m(u.machine);
		std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (m.find("aarch64") != std::string::npos || m.find("arm64") != std::string::npos) return "arm64";
		if (m.find("armv7") != std::string::npos || m.find("armv6") != std::string::npos || m.find("arm") != std::string::npos) return "arm";
		if (m.find("x86_64") != std::string::npos || m.find("amd64") != std::string::npos) return "x86_64";
		if (m.find("86") != std::string::npos || m.find("i386") != std::string::npos) return "x86";
		return "unknown";
#endif
		};

	auto arch = get_arch();

	auto filename_matches_arch = [&](const std::string& name_lower) -> bool {
		// detect architecture hints in filename
		if (name_lower.find("arm64") != std::string::npos || name_lower.find("aarch64") != std::string::npos) {
			return arch == "arm64";
		}
		if (name_lower.find("armv7") != std::string::npos || name_lower.find("armv6") != std::string::npos || name_lower.find("armhf") != std::string::npos || name_lower.find("arm") != std::string::npos) {
			return arch == "arm" || arch == "arm64";
		}
		if (name_lower.find("win64") != std::string::npos || name_lower.find("linux64") != std::string::npos || name_lower.find("x86_64") != std::string::npos || name_lower.find("amd64") != std::string::npos) {
			return arch == "x86_64" || arch == "arm64"; // arm64 systems may run amd64 binaries on some platforms (e.g. Windows via emulation), include arm64 too
		}
		if (name_lower.find("win32") != std::string::npos || name_lower.find("i386") != std::string::npos || name_lower.find("x86") != std::string::npos) {
			return arch == "x86" || arch == "x86_64";
		}
		// no arch hint -> assume generic
		return true;
		};
	for (const auto& f : files)
	{
		// 检查文件名是否与当前架构匹配
		std::string lower = f;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		// 如果当前是 Windows 或 Linux，过滤掉文件名中包含 "osx" 的压缩包
#if defined(BOOST_OS_WINDOWS) || defined(BOOST_OS_LINUX)
		if (lower.find("osx") != std::string::npos)
			continue;
#endif
		// 排除表示 alpha/beta/rc 或 版本后缀 a/b/c 的文件名后缀（在去掉 "blender" 之后）
		{
			std::string probe = lower;
			// remove occurrences of "blender" to avoid false positives
			size_t pos = 0;
			while ((pos = probe.find("blender", pos)) != std::string::npos) {
				probe.erase(pos, 7);
			}
			// check for "rc"
			if (probe.find("rc") != std::string::npos ||
				probe.find("preview") != std::string::npos ||
				probe.find("alpha") != std::string::npos ||
				probe.find("beta") != std::string::npos ||
				probe.find("add-ons-legacy-bundle") != std::string::npos) continue;
			// check for pattern: digit followed immediately by a/b/c
			bool foundSuffix = false;
			for (size_t i = 1; i < probe.size(); ++i) {
				if (std::isdigit(static_cast<unsigned char>(probe[i - 1]))) {
					char c = probe[i];
					if (c == 'a' || c == 'b' || c == 'c') { foundSuffix = true; break; }
				}
			}
			if (foundSuffix) continue;
		}
		if (!filename_matches_arch(lower)) continue;

		if (BOOST_OS_WINDOWS) {
			// Windows: 支持 .exe, .msi, .zip
			if (f.ends_with(".zip")) {
				available.push_back(f);
			}
		}
		if (BOOST_OS_LINUX) {
			// Linux: 支持 .tar.xz
			if (f.ends_with(".tar.xz") || f.ends_with(".zip")) {
				available.push_back(f);
			}
		}
		if (BOOST_OS_MACOS) {
			// macOS: 支持 .dmg 和 .zip
			if (f.ends_with(".dmg") || f.ends_with(".zip")) {
				available.push_back(f);
			}
		}
	}

	return available;
}

/// <summary>
/// 解析并显示可用版本信息
/// </summary>
/// <param name="versions">版本</param>
static void display_available_versions(const std::map<std::string, std::vector<std::string>>& versions)
{
	std::cout << "\n========== Available Blender Versions for ";
	std::cout << get_system_name() << " ==========\n\n";

	for (const auto& [version, files] : versions)
	{
		if (!files.empty())
		{
			std::cout << "Blender " << version << ":\n";
			for (const auto& file : files)
			{
				std::cout << "  - " << file << "\n";
			}
			std::cout << "\n";
		}
	}
}

/// <summary>
/// 智能推荐最佳版本
/// </summary>
/// <param name="versions">版本</param>
static void recommend_best_version(const std::map<std::string, std::vector<std::string>>& versions)
{
	if (versions.empty()) return;
	// pick latest by numeric version ordering
	std::vector<std::string> keys;
	for (const auto& kv : versions) keys.push_back(kv.first);
	std::sort(keys.begin(), keys.end(), version_less_numeric);
	auto latest = keys.empty() ? std::string() : keys.back();
	if (!latest.empty() && !versions.at(latest).empty())
	{
		std::cout << "⭐ 建议：最新稳定版本是Blender ";
		std::cout << latest << "\n";
		std::cout << "   Download: " << versions.at(latest)[0] << "\n";
		std::cout << "   URL: https://download.blender.org/release/Blender";
		std::cout << latest << "/" << versions.at(latest)[0] << "\n\n";
	}
}

/// <summary>
/// 输出系统信息
/// </summary>
static void print_sys_info()
{
	auto info = cpu_features::GetX86Info();

	std::cout << "========== 系统信息 ==========\n";
	std::cout << "Vendor: " << info.vendor << "\n";
	std::cout << "Family: " << info.family << "\n";
	std::cout << "Model:  " << info.model << "\n";
	std::cout << "System: " << get_system_name() << "\n";

	// 显示CPU特性
	if (info.features.avx2) {
		std::cout << "AVX2 supported ✓\n";
	}
	if (info.features.avx) {
		std::cout << "AVX supported ✓\n";
	}
	std::cout << "========================================\n\n";
}

/// <summary>
/// 主函数
/// </summary>
/// <returns></returns>
int main(int argc, char** argv)
{
	// parse command-line options using Boost.Program_options
	namespace po = boost::program_options;
	po::options_description desc("Allowed options");
	desc.add_options()
		("help,h", "show help")
		("show-all-ver", "list major versions")
		("show-small-ver", po::value<std::string>(), "list files for a major version (e.g. 3.6 or Blender3.6)")
		("download", po::value<std::string>(), "download file name or URL")
		("name", po::value<std::string>(), "custom name for extracted folder")
		("start", po::value<std::string>(), "start Blender installation (folder name or partial)");
	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	}
	catch (const std::exception& ex) {
		std::cerr << "Command line parse error: " << ex.what() << "\n";
		std::cerr << desc << "\n";
		return 1;
	}
	if (vm.count("help")) {
		std::cout << desc << "\n";
		return 0;
	}

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
	{
		std::cerr << "Failed to init curl global" << std::endl;
		return 1;
	}

	std::string html;
	if (!download_html("https://download.blender.org/release/", html, 8000))
	{
		std::cerr << "获取主索引失败\n";
		curl_global_cleanup();
		return 1;
	}

	GumboOutput* root = gumbo_parse(html.c_str());
	std::vector<std::string> majors;
	parse_major_versions(root->root, majors);
	gumbo_destroy_output(&kGumboDefaultOptions, root);

	// majors discovered (silent)

	// 存储每个版本的可用文件
	std::map<std::string, std::vector<std::string>> available_versions;

	// 并行抓取每个大版本，保证在 5 秒内完成（总体预算）
	const auto start_time = std::chrono::steady_clock::now();
	const std::chrono::milliseconds budget(5000);

	std::vector<std::future<std::pair<std::string, std::vector<std::string>>>> futures;
	for (const auto& ver : majors)
	{
		futures.emplace_back(std::async(std::launch::async, [ver, start_time, budget]() -> std::pair<std::string, std::vector<std::string>> {
			std::pair<std::string, std::vector<std::string>> result;
			result.first = ver;

			// 计算剩余时间
			auto now = std::chrono::steady_clock::now();
			long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_time + budget - now).count();
			if (remaining_ms <= 0) return result;

			std::string url = "https://download.blender.org/release/" + ver + "/";
			std::string sub_html;
			// 每个请求使用剩余时间的上限，但不超过 3000ms
			long timeout = static_cast<long>(std::min<long long>(remaining_ms, 3000LL));
			if (!download_html(url, sub_html, timeout))
			{
				// 失败或超时
				return result;
			}

			GumboOutput* sub = gumbo_parse(sub_html.c_str());
			std::set<std::string> files;
			parse_download_links(sub->root, files);
			gumbo_destroy_output(&kGumboDefaultOptions, sub);

			// 根据系统过滤文件
			auto system_files = filter_by_system(files);
			result.second = std::move(system_files);
			return result;
			}));
	}

	// Collect results, but stop respecting the 5s budget
	for (auto& f : futures)
	{
		// 计算剩余时间
		auto now = std::chrono::steady_clock::now();
		long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start_time + budget - now).count();
		if (remaining_ms <= 0)
		{
			std::cerr << "Time budget exceeded while gathering links\n";
			break;
		}

		// 等待带超时
		if (f.wait_for(std::chrono::milliseconds(remaining_ms)) == std::future_status::ready)
		{
			auto p = f.get();
			if (!p.second.empty())
			{
				available_versions[p.first] = p.second;
			}
		}
		else
		{
			std::cerr << "Future timed out for a version\n";
		}
	}

	// Ensure workspace and load config
	ensure_workspace_structure();
	SimpleConfig cfg = load_config_json();

	// 非交互式模式：支持下载和查询
	// --download=<file|url>: 下载指定文件（可为文件名或完整 URL）
	if (vm.count("download")) {
		std::string arg = vm["download"].as<std::string>();
		if (arg.empty()) {
			std::cerr << "--download requires a value, e.g. --download=blender-3.6.1-windows-x64.zip or --download=https://...\n";
			curl_global_cleanup();
			return 1;
		}

		std::string url;
		std::string filename;

		// If arg looks like a URL, use it directly
		if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
			url = arg;
			size_t pos = arg.find_last_of("/\\");
			filename = (pos == std::string::npos) ? arg : arg.substr(pos + 1);
		}
		else {
			// Search available_versions for exact match first
			std::string found_file;
			std::string found_version;
			for (const auto& kv : available_versions) {
				for (const auto& f : kv.second) {
					if (f == arg) { found_file = f; found_version = kv.first; break; }
				}
				if (!found_file.empty()) break;
			}
			// If not exact, try substring match
			if (found_file.empty()) {
				std::vector<std::pair<std::string, std::string>> candidates;
				for (const auto& kv : available_versions) {
					for (const auto& f : kv.second) {
						if (f.find(arg) != std::string::npos) candidates.emplace_back(kv.first, f);
					}
				}
				if (candidates.empty()) {
					std::cerr << "File not found in available versions: " << arg << "\n";
					curl_global_cleanup();
					return 1;
				}
				if (candidates.size() > 1) {
					std::cout << "Multiple matches found for '" << arg << "':\n";
					for (const auto& c : candidates) std::cout << "  " << c.first << "/" << c.second << "\n";
					curl_global_cleanup();
					return 1;
				}
				found_version = candidates[0].first;
				found_file = candidates[0].second;
			}

			url = std::string("https://download.blender.org/release/") + found_version + "/" + found_file;
			filename = found_file;
		}

		// determine exe directory (runtime directory) and ensure download directory exists under it
		std::filesystem::path exe_dir;
		try {
#if BOOST_OS_WINDOWS
			char buf[MAX_PATH];
			DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
			if (len > 0 && len < MAX_PATH) exe_dir = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path();
			else exe_dir = std::filesystem::current_path();
#else
			char buf[4096];
			ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
			if (len > 0) { exe_dir = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path(); }
			else exe_dir = std::filesystem::current_path();
#endif
		}
		catch (...) { exe_dir = std::filesystem::current_path(); }

		std::filesystem::path outdir = exe_dir / cfg.download_dir;
		try { std::filesystem::create_directories(outdir); }
		catch (...) {}
		std::filesystem::path outpath = outdir / filename;

		std::cout << "Downloading: " << url << " -> " << outpath.string() << "\n";
		bool ok = download_file(url, outpath, 0);
		if (!ok) {
			std::cerr << "Download failed for: " << url << "\n";
			curl_global_cleanup();
			return 1;
		}
		std::cout << "Downloaded to: " << outpath.string() << "\n";

		// If zip, extract and move to executable directory with name BlenderX.X.X
		if (outpath.extension() == ".zip") {
			// derive version string from filename
			std::string verstr;
			auto parts = parse_version_numbers(filename);
			if (!parts.empty()) {
				std::ostringstream vs;
				vs << parts[0];
				for (size_t i = 1; i < parts.size(); ++i) vs << "." << parts[i];
				verstr = vs.str();
			}
			if (verstr.empty()) {
				// fallback: try to find 'Blender' prefix + numbers in filename
				verstr = "";
			}

			// determine exe directory (runtime directory)
			std::filesystem::path exe_dir;
			try {
#if BOOST_OS_WINDOWS
				char buf[MAX_PATH];
				DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
				if (len > 0 && len < MAX_PATH) exe_dir = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path();
				else exe_dir = std::filesystem::current_path();
#else
				// try /proc/self/exe
				char buf[4096];
				ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
				if (len > 0) { exe_dir = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path(); }
				else exe_dir = std::filesystem::current_path();
#endif
			}
			catch (...) { exe_dir = std::filesystem::current_path(); }

			// allow overriding target folder name via --name option (Boost.Program_options)
			std::string target_name;
			if (vm.count("name")) target_name = vm["name"].as<std::string>();
			if (target_name.empty()) {
				target_name = std::string("Blender") + (verstr.empty() ? filename.substr(0, filename.find('.')) : verstr);
			}
			std::filesystem::path target_path = outdir / target_name;

			// temp extraction dir in download dir (under exe_dir/versions)
			std::filesystem::path tmpdir = outdir / (filename + std::string(".tmp_extract"));
			try { std::filesystem::remove_all(tmpdir); }
			catch (...) {}

			std::cout << "Extracting to temporary folder...\n";
			bool extracted = extract_zip(outpath, tmpdir);
			if (!extracted) {
				std::cerr << "Extraction failed for: " << outpath.string() << "\n";
				curl_global_cleanup();
				return 1;
			}

			// Inspect tmpdir children
			std::vector<std::filesystem::path> children;
			for (auto& e : std::filesystem::directory_iterator(tmpdir)) children.push_back(e.path());

			try {
				// remove existing target if present
				if (std::filesystem::exists(target_path)) {
					std::cout << "Removing existing target: " << target_path.string() << "\n";
					std::filesystem::remove_all(target_path);
				}

				if (children.size() == 1 && std::filesystem::is_directory(children[0])) {
					// rename single top-level directory to target name
					std::filesystem::rename(children[0], target_path);
				}
				else {
					// create target and move all children into it
					std::filesystem::create_directories(target_path);
					for (auto& p : children) {
						std::filesystem::path dest = target_path / p.filename();
						// try rename, fallback to copy
						try { std::filesystem::rename(p, dest); }
						catch (...) {
							std::filesystem::copy(p, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
							std::filesystem::remove_all(p);
						}
					}
				}
			}
			catch (const std::exception& ex) {
				std::cerr << "Failed to move extracted files: " << ex.what() << "\n";
				// cleanup tmpdir
				try { std::filesystem::remove_all(tmpdir); }
				catch (...) {}
				curl_global_cleanup();
				return 1;
			}

			// cleanup tmpdir
			try { std::filesystem::remove_all(tmpdir); }
			catch (...) {}

			// remove archive after successful extraction
			try { std::filesystem::remove(outpath); std::cout << "Removed archive: " << outpath.string() << "\n"; }
			catch (...) {}

			std::cout << "Extracted and moved to: " << target_path.string() << "\n";
		}

		curl_global_cleanup();
		return 0;
	}

	// --start=<BlenderName>: 启动指定已解压的 Blender 目录（文件夹名或部分匹配），支持同版本多实例
	if (vm.count("start")) {
		std::string arg = vm["start"].as<std::string>();
		if (arg.empty()) {
			std::cerr << "--start requires a value (folder name under versions), e.g. --start=Blender3.6.23 or partial match\n";
			curl_global_cleanup();
			return 1;
		}

		// determine exe_dir and versions dir
		std::filesystem::path exe_dir;
		try {
#if BOOST_OS_WINDOWS
			char buf[MAX_PATH];
			DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
			if (len > 0 && len < MAX_PATH) exe_dir = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path();
			else exe_dir = std::filesystem::current_path();
#else
			char buf[4096];
			ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
			if (len > 0) { exe_dir = std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path(); }
			else exe_dir = std::filesystem::current_path();
#endif
		}
		catch (...) { exe_dir = std::filesystem::current_path(); }

		std::filesystem::path versions_dir = exe_dir / cfg.download_dir;
		if (!std::filesystem::exists(versions_dir)) {
			std::cerr << "Versions directory not found: " << versions_dir.string() << "\n";
			curl_global_cleanup();
			return 1;
		}

		// find candidate folders
		std::vector<std::filesystem::path> candidates;
		for (auto& e : std::filesystem::directory_iterator(versions_dir)) {
			if (!e.is_directory()) continue;
			std::string name = e.path().filename().string();
			if (name == arg || name.find(arg) != std::string::npos) candidates.push_back(e.path());
		}

		if (candidates.empty()) {
			std::cerr << "No matching Blender installation found for: " << arg << "\n";
			curl_global_cleanup();
			return 1;
		}

		// launch each candidate
		for (auto& inst : candidates) {
			std::filesystem::path exe_path;
#if BOOST_OS_WINDOWS
			std::vector<std::string> look = { "blender.exe", "bin\\blender.exe" };
			for (auto& s : look) {
				std::filesystem::path p = inst / s;
				if (std::filesystem::exists(p)) { exe_path = p; break; }
			}
			if (exe_path.empty()) { std::cerr << "blender.exe not found in: " << inst.string() << "\n"; continue; }

			STARTUPINFOA si{}; PROCESS_INFORMATION pi{};
			si.cb = sizeof(si);
			std::string cmd = "\"" + exe_path.string() + "\"";
			if (!CreateProcessA(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP | CREATE_NEW_CONSOLE, NULL, inst.string().c_str(), &si, &pi)) {
				std::cerr << "Failed to start: " << exe_path.string() << " (err=" << GetLastError() << ")\n";
			}
			else {
				CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
				std::cout << "Started: " << exe_path.string() << "\n";
			}
#else
			std::vector<std::string> look = { "blender", "./blender", "bin/blender" };
			for (auto& s : look) {
				std::filesystem::path p = inst / s;
				if (std::filesystem::exists(p)) { exe_path = p; break; }
			}
			if (exe_path.empty()) { std::cerr << "blender not found in: " << inst.string() << "\n"; continue; }

			pid_t pid = fork();
			if (pid == 0) {
				exe_path = std::filesystem::absolute(exe_path);
				execl(exe_path.string().c_str(), exe_path.string().c_str(), (char*)NULL);
				_exit(1);
			}
			else if (pid > 0) {
				std::cout << "Started: " << exe_path.string() << " (pid=" << pid << ")\n";
			}
			else {
				std::cerr << "fork failed\n";
			}
#endif
		}

		curl_global_cleanup();
		return 0;
	}

	// --show-all-ver: 列出所有大版本
	if (vm.count("show-all-ver")) {
		std::cout << "Major versions:\n";
		for (const auto& m : majors) std::cout << "  " << m << "\n";
		curl_global_cleanup();
		return 0;
	}

	// --show-small-ver=<Major>: 列出指定大版本下可用的小版本（过滤当前系统）
	if (vm.count("show-small-ver")) {
		std::string arg = vm["show-small-ver"].as<std::string>();
		if (arg.empty()) {
			std::cerr << "--show-small-ver requires a value, e.g. --show-small-ver=3.6 or --show-small-ver=Blender3.6\n";
			curl_global_cleanup();
			return 1;
		}
		std::string target = arg;
		if (!(target.size() > 0 && (target[0] == 'B' || target[0] == 'b'))) target = std::string("Blender") + target;
		std::string url = std::string("https://download.blender.org/release/") + target + "/";
		std::string sub_html;
		if (!download_html(url, sub_html, 5000)) {
			std::cerr << "Failed to fetch: " << url << "\n";
			curl_global_cleanup();
			return 1;
		}
		GumboOutput* sub = gumbo_parse(sub_html.c_str());
		std::set<std::string> files_set;
		parse_download_links(sub->root, files_set);
		gumbo_destroy_output(&kGumboDefaultOptions, sub);
		auto files = filter_by_system(files_set);
		if (files.empty()) {
			std::cout << "No compatible files found for " << target << "\n";
			curl_global_cleanup();
			return 0;
		}
		std::sort(files.begin(), files.end(), filename_version_less);
		std::cout << "Files for " << target << ":\n";
		for (const auto& f : files) std::cout << "  " << f << "\n";
		curl_global_cleanup();
		return 0;
	}

	// 未提供已知非交互选项，打印由 Boost.Program_options 生成的用法提示
	std::cout << desc << "\n";
	curl_global_cleanup();
	return 0;
}