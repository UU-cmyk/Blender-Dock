#include "platform_utils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <clocale>
#include <boost/predef/os.h>
#if BOOST_OS_WINDOWS
#include <windows.h>
#elif BOOST_OS_LINUX
#include <unistd.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86) || defined(__i386__)
#include <cpu_features/cpuinfo_x86.h>
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
#include <cpu_features/cpuinfo_arm.h>
#elif defined(__powerpc__) || defined(__ppc__)
#include <cpu_features/cpuinfo_powerpc.h>
#endif

std::filesystem::path get_executable_dir()
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

std::string get_system_name()
{
	if (BOOST_OS_WINDOWS) {
		return "Windows";
	}
	else if (BOOST_OS_LINUX) {
		return "Linux";
	}
	else if (BOOST_OS_MACOS) {
		return "macOS";
	}
	else {
		return "Unknown OS";
	}
}

void print_sys_info()
{
	std::cout << "========== System Information ==========\n";
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

void ensure_workspace_structure()
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

std::string read_file_all(const std::filesystem::path& p)
{
	std::ifstream ifs(p);
	if (!ifs) return std::string();
	std::ostringstream ss;
	ss << ifs.rdbuf();
	return ss.str();
}

#if BOOST_OS_WINDOWS
std::string build_windows_command_line(const std::vector<std::string>& args)
{
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
