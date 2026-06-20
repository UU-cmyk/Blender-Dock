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

void handle_download(const po::variables_map& vm, Config& cfg) {
	// 懒惰地发现可用版本 (仅当请求下载时)
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

		// derive target folder name from version or filename
		std::string target_name;
		target_name = std::string("Blender") + (verstr.empty() ? filename.substr(0, filename.find('.')) : verstr);

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

void handle_delete(const po::variables_map& vm) {
	std::string arg = vm["delete"].as<std::string>();
	if (arg.empty()) {
		std::cerr << "--delete requires a value (folder name under versions), e.g. --delete=Blender3.6.23 or partial match\n";
		exit(1);
	}

	std::filesystem::path exe_dir = get_executable_dir();
	std::filesystem::path versions_dir = exe_dir / std::filesystem::path("versions");
	if (!std::filesystem::exists(versions_dir)) {
		std::cerr << "Versions directory not found: " << versions_dir.string() << "\n";
		exit(1);
	}

	// find matching folders
	std::vector<std::filesystem::path> to_delete;
	for (auto& e : std::filesystem::directory_iterator(versions_dir)) {
		if (!e.is_directory()) continue;
		std::string name = e.path().filename().string();
		if (name == arg || name.find(arg) != std::string::npos) to_delete.push_back(e.path());
	}

	if (to_delete.empty()) {
		std::cerr << "No matching Blender installation found for: " << arg << "\n";
		exit(1);
	}

	// confirm and delete
	std::cout << "The following Blender installations will be deleted:\n";
	for (const auto& p : to_delete) {
		std::cout << "  " << p.string() << "\n";
	}
	std::cout << "Are you sure? (y/N): ";
	std::string confirm;
	std::getline(std::cin, confirm);
	if (confirm != "y" && confirm != "Y") {
		std::cout << "Deletion cancelled.\n";
		return;
	}

	for (const auto& p : to_delete) {
		std::error_code ec;
		std::filesystem::remove_all(p, ec);
		if (ec) {
			std::cerr << "Failed to delete: " << p.string() << " (" << ec.message() << ")\n";
			exit(1);
		}
		std::cout << "Deleted: " << p.string() << "\n";
	}

	return;
}

void handle_rename(const po::variables_map& vm) {
	auto& vals = vm["rename"].as<std::vector<std::string>>();
	if (vals.size() < 2) {
		std::cerr << "--rename requires two arguments: <target-version> <new-name>, e.g. --rename Blender3.6 MyBlender\n";
		exit(1);
	}

	std::string target = vals[0];
	std::string new_name = vals[1];

	std::filesystem::path exe_dir = get_executable_dir();
	std::filesystem::path versions_dir = exe_dir / std::filesystem::path("versions");
	if (!std::filesystem::exists(versions_dir)) {
		std::cerr << "Versions directory not found: " << versions_dir.string() << "\n";
		exit(1);
	}

	// find matching folder (exact or substring match)
	std::vector<std::filesystem::path> candidates;
	for (auto& e : std::filesystem::directory_iterator(versions_dir)) {
		if (!e.is_directory()) continue;
		std::string name = e.path().filename().string();
		if (name == target || name.find(target) != std::string::npos) candidates.push_back(e.path());
	}

	if (candidates.empty()) {
		std::cerr << "No matching Blender installation found for: " << target << "\n";
		exit(1);
	}

	if (candidates.size() > 1) {
		std::cerr << "Multiple matches found for '" << target << "' - please be more specific:\n";
		for (const auto& p : candidates) std::cerr << "  " << p.filename().string() << "\n";
		exit(1);
	}

	std::filesystem::path old_path = candidates[0];
	std::filesystem::path new_path = old_path.parent_path() / new_name;

	if (std::filesystem::exists(new_path)) {
		std::cerr << "Target name already exists: " << new_path.string() << "\n";
		exit(1);
	}

	std::error_code ec;
	std::filesystem::rename(old_path, new_path, ec);
	if (ec) {
		std::cerr << "Failed to rename: " << old_path.string() << " -> " << new_path.string() << " (" << ec.message() << ")\n";
		exit(1);
	}

	std::cout << "Renamed: " << old_path.filename().string() << " -> " << new_name << "\n";
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
