#include "command_handlers.h"
#include "version_parser.h"
#include "version_discovery.h"
#include "network_client.h"
#include "download_utils.h"
#include "platform_utils.h"
#include "../zip_extractor.h"
#include "../config_manager.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cctype>
#include <boost/predef/os.h>
#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#endif

void handle_config(const po::variables_map& vm, Config& cfg) {
	auto& vals = vm["config"].as<std::vector<std::string>>();
	if (vals.empty()) {
		std::cerr << "--config requires arguments, e.g. --config download parallel_downloads=4\n";
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
			exit(1);
		}

		// use ConfigManager to update the field
		const std::string cfgpath = "config/config.json";
		ConfigManager::updateField(cfgpath, "parallel_downloads", new_parallel);

		// reload cfg
		cfg = ConfigManager::load(cfgpath);
		std::cout << "Updated config: " << cfgpath << " (parallel_downloads=" << new_parallel << ")\n";
		return;
	}
	else {
		std::cerr << "Unknown config section: " << section << "\n";
		exit(1);
	}
}

void handle_list_installed(const po::variables_map& vm) {
	// determine exe_dir and versions dir
	std::filesystem::path exe_dir = get_executable_dir();

	std::filesystem::path versions_dir = exe_dir / std::filesystem::path("versions");
	if (!std::filesystem::exists(versions_dir)) {
		std::cout << "Versions directory not found: " << versions_dir.string() << "\n";
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

	return;
}

void handle_assets(const po::variables_map& vm) {
	auto& vals = vm["assets"].as<std::vector<std::string>>();
	if (vals.empty()) {
		std::cerr << "--assets requires arguments, e.g. --assets add MyLib C:/path/to/lib or --assets list\n";
		exit(1);
	}

	// 定位脚本路径
	std::filesystem::path script_path;
	std::filesystem::path exe_dir = get_executable_dir();
	std::filesystem::path cand1 = exe_dir / "bpy" / "blender_assets.py";
	std::filesystem::path cand2 = std::filesystem::current_path() / "bpy" / "blender_assets.py";
	if (std::filesystem::exists(cand1)) script_path = cand1;
	else if (std::filesystem::exists(cand2)) script_path = cand2;
	else {
		std::cerr << "Blender assets script not found (expected bpy/blender_assets.py in exe or cwd)\n";
		exit(1);
	}

	// 解析参数，提取 blender 可执行文件和传递给脚本的参数
	std::string blender_exe = "blender";   // 默认从 PATH 查找
	std::vector<std::string> pass_args;
	for (size_t i = 0; i < vals.size(); ++i) {
		const auto& tok = vals[i];
		if ((tok == "--target-blender" || tok == "--blender-exe") && (i + 1) < vals.size()) {
			blender_exe = vals[i + 1];
			++i; // 跳过下一个 token
		}
		else {
			pass_args.push_back(tok);
		}
	}

	// 构建参数字符串列表（不含 shell）
	std::vector<std::string> args;
	args.push_back(blender_exe);
	args.push_back("--background");
	args.push_back("--python");
	args.push_back(script_path.string());
	args.push_back("--");
	for (const auto& a : pass_args) {
		args.push_back(a);
	}

	// ---------- 平台相关进程创建与同步等待 ----------
	int exit_code = 1;
	// 约定 Python 脚本将结果写入此文件
	const std::string result_file = "assets_result.txt";

#if BOOST_OS_WINDOWS
	// Windows：使用 CreateProcess，重定向子进程标准输出和标准错误到 NUL
	std::string cmdline = build_windows_command_line(args);
	std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
	cmdline_buf.push_back('\0');

	// 创建 NUL 句柄（类似 /dev/null）
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE hNull = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, NULL);
	if (hNull == INVALID_HANDLE_VALUE) {
		std::cerr << "Failed to open NUL device\n";
		exit(1);
	}

	STARTUPINFOA si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = hNull;
	si.hStdError = hNull;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};

	BOOL success = CreateProcessA(
		nullptr,
		cmdline_buf.data(),
		nullptr,
		nullptr,
		TRUE,
		0,
		nullptr,
		nullptr,
		&si,
		&pi
	);

	CloseHandle(hNull);

	if (!success) {
		std::cerr << "Failed to create process: " << blender_exe << " (error " << GetLastError() << ")\n";
		exit(1);
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code_win = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code_win);
	exit_code = static_cast<int>(exit_code_win);

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

#else
	// POSIX (Linux / macOS)：使用 fork + execvp，重定向到 /dev/null
	pid_t pid = fork();
	if (pid == 0) {
		int null_fd = open("/dev/null", O_WRONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			close(null_fd);
		}
		std::vector<char*> argv;
		for (auto& a : args) {
			argv.push_back(const_cast<char*>(a.c_str()));
		}
		argv.push_back(nullptr);

		execvp(argv[0], argv.data());
		std::cerr << "execvp failed: " << strerror(errno) << std::endl;
		_exit(1);
	}
	else if (pid > 0) {
		int status = 0;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status)) {
			exit_code = WEXITSTATUS(status);
		}
		else {
			std::cerr << "Blender process terminated abnormally\n";
			exit_code = 1;
		}
	}
	else {
		std::cerr << "fork failed\n";
		exit(1);
	}
#endif

	if (exit_code != 0) {
		std::cerr << "Blender asset management script exited with code " << exit_code << "\n";
		exit(1);
	}

	// 读取 Python 脚本生成的结果文件并输出到控制台
	std::ifstream ifs(result_file);
	if (ifs) {
		std::string line;
		while (std::getline(ifs, line)) {
			std::cout << line << '\n';
		}
		ifs.close();
		std::filesystem::remove(result_file);
	}

	return;
}

void handle_download(const po::variables_map& vm, Config& cfg) {
	// discover available versions lazily (only when download is requested)
	VersionDiscovery vd;
	auto available_versions = vd.discover("https://download.blender.org/release/", 5000);

	std::string arg = vm["download"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--download requires a value, e.g. --download=blender-3.6.1-windows-x64.zip or --download=https://...\n";
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

				std::filesystem::path exe_dir_local = get_executable_dir();
				std::filesystem::path outdir_local = exe_dir_local / std::filesystem::path("versions");
				try { std::filesystem::create_directories(outdir_local); }
				catch (...) {}

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

				unsigned int workers_to_launch = std::min<unsigned int>(parallel, static_cast<unsigned int>(tasks.size()));
				for (unsigned int w = 0; w < workers_to_launch; ++w) workers.emplace_back(worker, w);
				for (auto& th : workers) if (th.joinable()) th.join();

				bool any_failed = false;
				for (size_t i = 0; i < results.size(); ++i) if (!results[i]) any_failed = true;

				if (any_failed) {
					std::cerr << "One or more downloads failed\n";
					exit(1);
				}
				else {
					std::cout << "All downloads completed successfully\n";
					return;
				}
			}

			found_version = candidates[0].first;
			found_file = candidates[0].second;
		}

		url = std::string("https://download.blender.org/release/") + found_version + "/" + found_file;
		filename = found_file;
	}

	// determine exe directory and ensure download directory exists under it
	std::filesystem::path exe_dir = get_executable_dir();
	std::filesystem::path outdir = exe_dir / std::filesystem::path("versions");
	try { std::filesystem::create_directories(outdir); }
	catch (...) {}
	std::filesystem::path outpath = outdir / filename;

	std::cout << "Downloading: " << url << " -> " << outpath.string() << "\n";
	bool ok = download_file(url, outpath, 0);
	if (!ok) {
		std::cerr << "Download failed for: " << url << "\n";
		exit(1);
	}
	std::cout << "Downloaded to: " << outpath.string() << "\n";

	// If zip, extract and organize using ZipExtractor
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

		// allow overriding target folder name via --name option
		std::string target_name;
		if (vm.count("name")) target_name = vm["name"].as<std::string>();
		if (target_name.empty()) {
			target_name = std::string("Blender") + (verstr.empty() ? filename.substr(0, filename.find('.')) : verstr);
		}

		std::cout << "Extracting to " << target_name << "...\n";
		bool extracted = ZipExtractor::extractAndOrganize(outpath, outdir, target_name);
		if (!extracted) {
			std::cerr << "Extraction failed for: " << outpath.string() << "\n";
			exit(1);
		}
		std::cout << "Extracted and moved to: " << (outdir / target_name).string() << "\n";
	}

	return;
}

void handle_start(const po::variables_map& vm) {
	std::string arg = vm["start"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--start requires a value (folder name under versions), e.g. --start=Blender3.6.23 or partial match\n";
		exit(1);
	}

	std::filesystem::path exe_dir = get_executable_dir();
	std::filesystem::path versions_dir = exe_dir / std::filesystem::path("versions");
	if (!std::filesystem::exists(versions_dir)) {
		std::cerr << "Versions directory not found: " << versions_dir.string() << "\n";
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

	return;
}

void handle_show_all_ver() {
	std::string html;
	if (!download_html("https://download.blender.org/release/", html, 8000)) {
		std::cerr << "Failed to fetch main index\n";
		exit(1);
	}
	VersionParser vp_for_major;
	std::vector<std::string> majors = vp_for_major.parseMajorVersions(html);
	std::cout << "Major versions:\n";
	for (const auto& m : majors) std::cout << "  " << m << "\n";
	return;
}

void handle_show_small_ver(const po::variables_map& vm) {
	std::string arg = vm["show-small-ver"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--show-small-ver requires a value, e.g. --show-small-ver=3.6 or --show-small-ver=Blender3.6\n";
		exit(1);
	}
	std::string target = arg;
	if (!(target.size() > 0 && (target[0] == 'B' || target[0] == 'b'))) target = std::string("Blender") + target;
	std::string url = std::string("https://download.blender.org/release/") + target + "/";
	std::string sub_html;
	if (!download_html(url, sub_html, 5000)) {
		std::cerr << "Failed to fetch: " << url << "\n";
		exit(1);
	}
	VersionParser vp_local;
	std::set<std::string> files_set = vp_local.parseDownloadLinks(sub_html);
	auto files = vp_local.filterBySystem(files_set);
	if (files.empty()) {
		std::cout << "No compatible files found for " << target << "\n";
		return;
	}
	std::sort(files.begin(), files.end(), filename_version_less);
	std::cout << "Files for " << target << ":\n";
	for (const auto& f : files) std::cout << "  " << f << "\n";
	return;
}
