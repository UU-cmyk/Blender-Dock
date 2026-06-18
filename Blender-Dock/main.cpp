#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <iostream>
#include <cstdlib>
#include <curl/curl.h>
#include <boost/program_options.hpp>
#include "config_manager.h"
#include "Blender-Dock/command_handlers.h"
#include "Blender-Dock/platform_utils.h"

namespace po = boost::program_options;

// 调用真正的 libcurl 清理的包装器我们将使用 atexit 注册它
static void curl_global_cleanup_impl() { curl_global_cleanup(); }

int main(int argc, char** argv)
{
	// 使用 Boost Program 解析命令选项
	po::options_description desc("全部选项");
	desc.add_options()
		("help,h", "显示帮助")
		("config", po::value<std::vector<std::string>>()->multitoken(), "配置设置：例如 --config download parallel_downloads=4")
		("show-all-ver", "列出主版本号")
		("show-small-ver", po::value<std::string>(), "列出某个主版本的文件 (例如 3.6 或 Blender3.6)")
		("download", po::value<std::string>(), "下载文件名或 URL")
		("name", po::value<std::string>(), "解压文件夹的自定义名称")
		("start", po::value<std::string>(), "启动 Blender 安装 (文件夹名称或部分匹配)")
		("list-installed", "列出本地版本目录下已安装的 Blender 版本")
		("delete", po::value<std::string>(), "删除已安装的 Blender 版本 (文件夹名称或部分匹配) ");
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

	std::atexit(curl_global_cleanup_impl);

	// 确保工作区和加载配置
	ensure_workspace_structure();
	Config cfg = ConfigManager::load();

	// --config: 配置修改
	if (vm.count("config")) handle_config(vm, cfg);

	// --list-installed: 列出安装的Blender版本
	if (vm.count("list-installed")) handle_list_installed(vm);

	// --delete: 删除已安装的Blender版本
	if (vm.count("delete")) handle_delete(vm);

	// --download: download a file or URL
	if (vm.count("download")) handle_download(vm, cfg);

	// --start: launch a specified Blender installation
	if (vm.count("start")) handle_start(vm);

	// --show-all-ver: list all major versions
	if (vm.count("show-all-ver")) handle_show_all_ver();

	// --show-small-ver: list available files for a major version
	if (vm.count("show-small-ver")) handle_show_small_ver(vm);

	// If no recognized option was given, print usage
	if (!(vm.count("config") ||
		vm.count("list-installed") ||
		vm.count("delete") ||
		vm.count("download") ||
		vm.count("start") ||
		vm.count("show-all-ver") ||
		vm.count("show-small-ver"))) {
		std::cout << desc << "\n";
	}
	return 0;
}