#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <curl/curl.h>
#include "Blender-Dock/version_parser.h"
#include "Blender-Dock/version_discovery.h"
#include "Blender-Dock/network_client.h"
#include <thread>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <cstdlib>
#include <clocale>
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
#if BOOST_OS_WINDOWS
#include <windows.h>
#elif BOOST_OS_LINUX
#include <unistd.h>
#endif
#include <boost/predef/os.h>
#include <algorithm>
#include <cctype>
#include <zip.h>
#include <nlohmann/json.hpp>

namespace po = boost::program_options;

// 前向声明：下面会使用 parse_version_numbers
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

// 从文件加载配置（简单密钥格式返回下载基目录
// 简单的 JSON 配置加载器（无外部 JSON 依赖项）
struct SimpleConfig {
	unsigned int parallel = 0; // 0 表示自动（使用硬件并发）
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

/// <summary>
/// Writes the default configuration.
/// </summary>
/// <param name="cfgpath">The cfgpath.</param>
static void write_default_config(const std::filesystem::path& cfgpath)
{
	try {
		std::filesystem::create_directories(cfgpath.parent_path());
		nlohmann::json j;
		j["parallel_downloads"] = 0;
		std::ofstream ofs(cfgpath, std::ios::trunc);
		if (!ofs) return;
		ofs << j.dump(2) << std::endl;
	}
	catch (...) {}
}

static SimpleConfig load_config_json(const std::string& cfgfile = "config/config.json")
{
	SimpleConfig cfg;
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

	try {
		auto j = nlohmann::json::parse(s);
		if (j.contains("parallel_downloads") && j["parallel_downloads"].is_number_integer()) cfg.parallel = static_cast<unsigned int>(j["parallel_downloads"].get<int>());
		if (j.contains("auto_extract_addons")) cfg.auto_extract_addons = j["auto_extract_addons"].get<bool>();
		if (j.contains("auto_link_addons")) cfg.auto_link_addons = j["auto_link_addons"].get<bool>();
		if (j.contains("blender_user_dir") && j["blender_user_dir"].is_string()) cfg.blender_user_dir = j["blender_user_dir"].get<std::string>();
	}
	catch (...) {
		// failed to parse JSON, return defaults
	}

	return cfg;
}

// 确保工作区文件夹结构存在
static void ensure_workspace_structure()
{
	try {
		std::filesystem::create_directories("config");
		std::filesystem::create_directories("versions");
		std::filesystem::create_directories("addons");
		std::filesystem::create_directories("assets");
		std::filesystem::create_directories("logs");
	}
	catch (...) {}
}

// 返回可执行文件所在目录（平台特定实现）
static std::filesystem::path get_executable_dir()
{
	try {
#if BOOST_OS_WINDOWS
		char buf[MAX_PATH];
		DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
		if (len > 0 && len < MAX_PATH) return std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path();
		return std::filesystem::current_path();
#else
		char buf[4096];
		ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (len > 0) return std::filesystem::path(std::string(buf, static_cast<size_t>(len))).parent_path();
		return std::filesystem::current_path();
#endif
	}
	catch (...) {
		return std::filesystem::current_path();
	}
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

/// <summary>
/// 下载文件到指定路径（覆盖）
/// </summary>
/// <param name="url">目标URL</param>
/// <param name="out_path">目标路径</param>
/// <param name="timeout_ms">The timeout ms.</param>
/// <returns></returns>
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
/// 输出系统信息
/// </summary>
static void print_sys_info()
{
	std::cout << "========== 系统信息 ==========\n";
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86) || defined(__i386__)
	const auto info = cpu_features::GetX86Info();

	std::cout << "Vendor: " << info.vendor << "\n";
	std::cout << "Family: " << info.family << "\n";
	std::cout << "Model:  " << info.model << "\n";
	std::cout << "System: " << get_system_name() << "\n";

	if (info.features.avx2) std::cout << "AVX2 supported ✓\n";
	if (info.features.avx) std::cout << "AVX supported ✓\n";
#elif defined(__aarch64__) || defined(_M_ARM64)
	const auto info = cpu_features::GetArmInfo();

	std::cout << "Architecture: ARM64\n";
	if (info.features.asimd) std::cout << "ASIMD supported ✓\n";
	if (info.features.neon)  std::cout << "NEON supported ✓\n";
	if (info.features.crc32) std::cout << "CRC32 supported ✓\n";
#elif defined(__arm__) || defined(_M_ARM)
	const auto info = cpu_features::GetArmInfo();

	std::cout << "Architecture: ARM32\n";
	if (info.features.neon) std::cout << "NEON supported ✓\n";
#elif defined(__powerpc__) || defined(__ppc__)
	const auto info = cpu_features::GetPowerpcInfo();

	std::cout << "Architecture: PowerPC\n";
	if (info.features.altivec) std::cout << "AltiVec supported ✓\n";
#endif

	std::cout << "System: " << get_system_name() << "\n";
	std::cout << "========================================\n\n";
}

#if BOOST_OS_WINDOWS
static std::string build_windows_command_line(const std::vector<std::string>& args) {
	std::string cmdline;
	for (size_t i = 0; i < args.size(); ++i) {
		if (i > 0) cmdline += ' ';
		const std::string& arg = args[i];
		// 需要转义的情况：参数包含空格、制表符或双引号
		bool need_quote = (arg.find_first_of(" \t\"") != std::string::npos);
		if (!need_quote) {
			cmdline += arg;
		}
		else {
			cmdline += '"';
			for (char c : arg) {
				if (c == '"') {
					cmdline += "\\\"";   // 内部双引号转义
				}
				else {
					cmdline += c;
				}
			}
			cmdline += '"';
		}
	}
	return cmdline;
}
#endif

static void handle_config(const po::variables_map& vm, SimpleConfig& cfg) {
	auto& vals = vm["config"].as<std::vector<std::string>>();
	if (vals.empty()) {
		std::cerr << "--config requires arguments, e.g. --config download parallel_downloads=4\n";
		curl_global_cleanup();
		exit(1);
	}
	std::string section = vals[0];
	if (section == "download") {
		int new_parallel = -1;
		for (size_t i = 1; i < vals.size(); ++i) {
			const std::string& tok = vals[i];
			const std::string key = "parallel_downloads=";
			if (tok.rfind(key, 0) == 0) {
				std::string num = tok.substr(key.size());
				try { new_parallel = std::stoi(num); }
				catch (...) { new_parallel = -1; }
			}
		}
		if (new_parallel < 0) {
			std::cerr << "Invalid or missing parallel_downloads value. Usage: --config download parallel_downloads=4\n";
			curl_global_cleanup();
			exit(1);
		}

		// read config file
		std::filesystem::path cfgpath = std::filesystem::path("config") / "config.json";
		std::string s = read_file_all(cfgpath);
		if (s.empty()) {
			// create default then read again
			write_default_config(cfgpath);
			s = read_file_all(cfgpath);
		}
		if (s.empty()) s = "{}";

		// locate existing key
		std::string keyname = "\"parallel_downloads\"";
		size_t pos = s.find(keyname);
		if (pos != std::string::npos) {
			size_t colon = s.find(':', pos);
			if (colon != std::string::npos) {
				size_t valstart = colon + 1;
				// skip spaces
				while (valstart < s.size() && isspace(static_cast<unsigned char>(s[valstart]))) ++valstart;
				size_t valend = valstart;
				while (valend < s.size() && (std::isdigit(static_cast<unsigned char>(s[valend])) || s[valend] == '-')) ++valend;
				s.replace(valstart, valend - valstart, std::to_string(new_parallel));
			}
		}
		else {
			// insert before final }
			size_t brace = s.rfind('}');
			std::string insert = "  \"parallel_downloads\": " + std::to_string(new_parallel) + "\n";
			if (brace != std::string::npos) {
				// ensure proper comma handling: if preceding non-space before brace is not '{', add comma
				size_t prev = brace;
				while (prev > 0 && isspace(static_cast<unsigned char>(s[prev - 1]))) --prev;
				if (prev > 0 && s[prev - 1] != '{' && s[prev - 1] != ',') insert = ",\n" + insert;
				s.insert(brace, insert);
			}
			else {
				s = "{\n" + insert + "}\n";
			}
		}

		// write back
		try {
			std::filesystem::create_directories(cfgpath.parent_path());
			std::ofstream ofs(cfgpath, std::ios::trunc);
			if (!ofs) {
				std::cerr << "Failed to open config file for writing: " << cfgpath.string() << "\n";
				curl_global_cleanup();
				exit(1);
			}
			ofs << s;
			ofs.close();
			std::cout << "Updated config: " << cfgpath.string() << " (parallel_downloads=" << new_parallel << ")\n";
		}
		catch (const std::exception& ex) {
			std::cerr << "Failed to write config: " << ex.what() << "\n";
			curl_global_cleanup();
			exit(1);
		}

		// reload cfg
		cfg = load_config_json(cfgpath.string());
		curl_global_cleanup();
		return;
	}

	else {
		std::cerr << "Unknown config section: " << section << "\n";
		curl_global_cleanup();
		exit(1);
	}
}

static void handle_list_installed(const po::variables_map& vm) {
	// determine exe_dir and versions dir
	std::filesystem::path exe_dir = get_executable_dir();

	std::filesystem::path versions_dir = exe_dir / std::filesystem::path("versions");
	if (!std::filesystem::exists(versions_dir)) {
		std::cout << "Versions directory not found: " << versions_dir.string() << "\n";
		curl_global_cleanup();
		return;
	}

	std::cout << "Installed Blender versions under: " << versions_dir.string() << "\n";
	for (auto& e : std::filesystem::directory_iterator(versions_dir)) {
		if (!e.is_directory()) continue;
		std::string name = e.path().filename().string();
		std::filesystem::path inst = e.path();
		// locate blender executable
		std::filesystem::path exe_path;
#if BOOST_OS_WINDOWS
		std::vector<std::string> look = { "blender.exe", "bin\\blender.exe" };
		for (auto& s : look) {
			std::filesystem::path p = inst / s;
			if (std::filesystem::exists(p)) { exe_path = p; break; }
		}
#else
		std::vector<std::string> look = { "blender", "./blender", "bin/blender" };
		for (auto& s : look) {
			std::filesystem::path p = inst / s;
			if (std::filesystem::exists(p)) { exe_path = p; break; }
		}
#endif
		std::cout << " - " << name << " -> " << inst.string();
		if (!exe_path.empty()) std::cout << " (executable: " << exe_path.string() << ")";
		else std::cout << " (no executable found)";
		std::cout << "\n";
	}

	curl_global_cleanup();
	return;
}

static void handle_assets(const po::variables_map& vm) {
	auto& vals = vm["assets"].as<std::vector<std::string>>();
	if (vals.empty()) {
		std::cerr << "--assets requires arguments, e.g. --assets add MyLib C:/path/to/lib or --assets list\n";
		curl_global_cleanup();
		exit(1);;
	}

	// locate script: try executable dir and current working dir
	std::filesystem::path script_path;
	std::filesystem::path exe_dir = get_executable_dir();

	std::filesystem::path cand1 = exe_dir / "bpy" / "blender_assets.py";
	std::filesystem::path cand2 = std::filesystem::current_path() / "bpy" / "blender_assets.py";
	if (std::filesystem::exists(cand1)) script_path = cand1;
	else if (std::filesystem::exists(cand2)) script_path = cand2;
	else {
		std::cerr << "Blender assets script not found (expected bpy/blender_assets.py in exe or cwd)\n";
		curl_global_cleanup();
		exit(1);;
	}

	// build command
	// 支持在 --assets 参数中指定特殊选项 --target-blender <path> 来指定要调用的 Blender 可执行文件
	std::string blender_exe = "blender";
	std::vector<std::string> pass_args;
	for (size_t i = 0; i < vals.size(); ++i) {
		const auto& tok = vals[i];
		if ((tok == "--target-blender" || tok == "--blender-exe") && (i + 1) < vals.size()) {
			blender_exe = vals[i + 1];
			++i; // skip next token
		}
		else {
			pass_args.push_back(tok);
		}
	}

	// quote blender executable if necessary
	std::string cmd;
	if (blender_exe.find(' ') != std::string::npos) cmd = "\"" + blender_exe + "\"";
	else cmd = blender_exe;
	cmd += " --background --python \"" + script_path.string() + "\" --";

	for (const auto& s : pass_args) {
		// quote each argument
		std::string q = s;
		if (q.find(' ') != std::string::npos) cmd += " \"" + q + "\"";
		else cmd += " " + q;
	}

	std::cout << "Invoking: " << cmd << "\n";
	int rc = std::system(cmd.c_str());
	if (rc != 0) {
		std::cerr << "Failed to run blender for asset management (rc=" << rc << ")\n";
		curl_global_cleanup();
		exit(1);;
	}

	curl_global_cleanup();
	return;
}

static void handle_download(const po::variables_map& vm, SimpleConfig& cfg) {
	// discover available versions lazily (only when download is requested)
	VersionDiscovery vd;
	auto available_versions = vd.discover("https://download.blender.org/release/", 5000);

	std::string arg = vm["download"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--download requires a value, e.g. --download=blender-3.6.1-windows-x64.zip or --download=https://...\n";
		curl_global_cleanup();
		exit(1);
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
				exit(1);
			}

			if (candidates.size() > 1) {
				// Multiple matching files: perform parallel downloads according to config
				unsigned int parallel = cfg.parallel;
				if (parallel == 0) {
					parallel = std::thread::hardware_concurrency();
					if (parallel == 0) parallel = 2;
				}

				std::cout << "Multiple matches found for '" << arg << "' - downloading " << candidates.size() << " files using " << parallel << " threads\n";

				// determine exe directory (runtime directory) and ensure download directory exists under it
				std::filesystem::path exe_dir_local = get_executable_dir();

				std::filesystem::path outdir_local = exe_dir_local / std::filesystem::path("versions");
				try { std::filesystem::create_directories(outdir_local); }
				catch (...) {}

				// prepare list of urls and filenames
				std::vector<std::pair<std::string, std::string>> tasks;
				for (const auto& c : candidates) {
					std::string ver = c.first;
					std::string fn = c.second;
					std::string u = std::string("https://download.blender.org/release/") + ver + "/" + fn;
					tasks.emplace_back(u, fn);
				}

				std::atomic_size_t index{ 0 };
				std::vector<std::thread> workers;
				std::vector<bool> results(tasks.size(), false);
				std::mutex io_mtx;

				auto worker = [&](unsigned int /*id*/) {
					for (;;) {
						size_t i = index.fetch_add(1);
						if (i >= tasks.size()) break;
						const auto& t = tasks[i];
						std::filesystem::path outpath = outdir_local / t.second;

						{
							std::lock_guard<std::mutex> lk(io_mtx);
							std::cout << "[thread] Downloading: " << t.first << " -> " << outpath.string() << "\n";
						}

						bool ok = download_file(t.first, outpath, 0);

						{
							std::lock_guard<std::mutex> lk(io_mtx);
							if (ok) std::cout << "[thread] Downloaded: " << outpath.string() << "\n";
							else std::cerr << "[thread] Failed: " << t.first << "\n";
						}
						results[i] = ok;
					}
					};

				// launch workers
				unsigned int workers_to_launch = std::min<unsigned int>(parallel, static_cast<unsigned int>(tasks.size()));
				for (unsigned int w = 0; w < workers_to_launch; ++w) workers.emplace_back(worker, w);
				for (auto& th : workers) if (th.joinable()) th.join();

				bool any_failed = false;
				for (size_t i = 0; i < results.size(); ++i) if (!results[i]) any_failed = true;

				if (any_failed) {
					std::cerr << "One or more downloads failed\n";
					curl_global_cleanup();
					exit(1);
				}
				else {
					std::cout << "All downloads completed successfully\n";
					curl_global_cleanup();
					return;
				}
			}

			found_version = candidates[0].first;
			found_file = candidates[0].second;
		}

		url = std::string("https://download.blender.org/release/") + found_version + "/" + found_file;
		filename = found_file;
	}

	// determine exe directory (runtime directory) and ensure download directory exists under it
	std::filesystem::path exe_dir = get_executable_dir();

	std::filesystem::path outdir = exe_dir / std::filesystem::path("versions");
	try { std::filesystem::create_directories(outdir); }
	catch (...) {}
	std::filesystem::path outpath = outdir / filename;

	std::cout << "Downloading: " << url << " -> " << outpath.string() << "\n";
	bool ok = download_file(url, outpath, 0);
	if (!ok) {
		std::cerr << "Download failed for: " << url << "\n";
		curl_global_cleanup();
		exit(1);
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

		// determine exe directory (runtime directory)
		std::filesystem::path exe_dir = get_executable_dir();

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
			exit(1);
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
			exit(1);
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
	return;
}

static void handle_start(const po::variables_map& vm) {
	std::string arg = vm["start"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--start requires a value (folder name under versions), e.g. --start=Blender3.6.23 or partial match\n";
		curl_global_cleanup();
		exit(1);
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

	std::filesystem::path versions_dir = exe_dir / std::filesystem::path("versions");
	if (!std::filesystem::exists(versions_dir)) {
		std::cerr << "Versions directory not found: " << versions_dir.string() << "\n";
		curl_global_cleanup();
		exit(1);
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
		exit(1);
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
	return;
}

static void handle_show_all_ver() {
	std::string html;
	if (!download_html("https://download.blender.org/release/", html, 8000)) {
		std::cerr << "获取主索引失败\n";
		curl_global_cleanup();
		exit(1);
	}
	VersionParser vp_for_major;
	std::vector<std::string> majors = vp_for_major.parseMajorVersions(html);
	std::cout << "Major versions:\n";
	for (const auto& m : majors) std::cout << "  " << m << "\n";
	curl_global_cleanup();
	return;
}

static void handle_show_small_ver(const po::variables_map& vm) {
	std::string arg = vm["show-small-ver"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--show-small-ver requires a value, e.g. --show-small-ver=3.6 or --show-small-ver=Blender3.6\n";
		curl_global_cleanup();
		exit(1);
	}
	std::string target = arg;
	if (!(target.size() > 0 && (target[0] == 'B' || target[0] == 'b'))) target = std::string("Blender") + target;
	std::string url = std::string("https://download.blender.org/release/") + target + "/";
	std::string sub_html;
	if (!download_html(url, sub_html, 5000)) {
		std::cerr << "Failed to fetch: " << url << "\n";
		curl_global_cleanup();
		exit(1);
	}
	VersionParser vp_local;
	std::set<std::string> files_set = vp_local.parseDownloadLinks(sub_html);
	auto files = vp_local.filterBySystem(files_set);
	if (files.empty()) {
		std::cout << "No compatible files found for " << target << "\n";
		curl_global_cleanup();
		return;
	}
	std::sort(files.begin(), files.end(), filename_version_less);
	std::cout << "Files for " << target << ":\n";
	for (const auto& f : files) std::cout << "  " << f << "\n";
	curl_global_cleanup();
	return;
}

/// <summary>
/// 主函数
/// </summary>
/// <returns></returns>
int main(int argc, char** argv)
{
	// parse command-line options using Boost.Program_options
	po::options_description desc("全部选项");
	desc.add_options()
		("help,h", "显示帮助")
		("assets", po::value<std::vector<std::string>>()->multitoken(), "管理 Blender 资产库：添加/移除/修改/列出 (传递给 bpy/blender_assets.py)")
		("config", po::value<std::vector<std::string>>()->multitoken(), "配置设置：例如 --config download parallel_downloads=4")
		("show-all-ver", "列出主版本号")
		("show-small-ver", po::value<std::string>(), "列出某个主版本的文件 (例如 3.6 或 Blender3.6)")
		("download", po::value<std::string>(), "下载文件名或 URL")
		("name", po::value<std::string>(), "解压文件夹的自定义名称")
		("start", po::value<std::string>(), "启动 Blender 安装 (文件夹名称或部分匹配)")
		("list-installed", "列出本地版本目录下已安装的 Blender 版本");
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

	std::atexit([] { curl_global_cleanup(); });

	// Ensure workspace and load config
	ensure_workspace_structure();
	SimpleConfig cfg = load_config_json();

	// --config: configuration modifications (e.g. --config download parallel_downloads=4)
	if (vm.count("config")) handle_config(vm, cfg);

	// --list-installed: 列出本地 versions 目录下已安装的 Blender
	if (vm.count("list-installed")) handle_list_installed(vm);

	// --assets: manage Blender asset libraries by invoking the bundled bpy/blender_assets.py
	if (vm.count("assets")) handle_assets(vm);

	// 非交互式模式：支持下载和查询
	// --download=<file|url>: 下载指定文件（可为文件名或完整 URL）
	if (vm.count("download")) handle_download(vm, cfg);

	// --start=<BlenderName>: 启动指定已解压的 Blender 目录（文件夹名或部分匹配），支持同版本多实例
	if (vm.count("start")) handle_start(vm);

	// --show-all-ver: 列出所有大版本
	if (vm.count("show-all-ver")) handle_show_all_ver();

	// --show-small-ver=<Major>: 列出指定大版本下可用的小版本（过滤当前系统）
	if (vm.count("show-small-ver")) handle_show_small_ver(vm);

	// 未提供已知非交互选项，打印由 Boost.Program_options 生成的用法提示
	if (!(vm.count("config") ||
		vm.count("list-installed") ||
		vm.count("assets") ||
		vm.count("download") ||
		vm.count("start") ||
		vm.count("show-all-ver") ||
		vm.count("show-small-ver"))) {
		std::cout << desc << "\n";
	}
	return 0;
}