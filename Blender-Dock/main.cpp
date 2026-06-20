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
		("name", po::value<std::string>(), "自定义名称（独立选项，也可与 --download/--start/--delete 组合使用）")
		("start", po::value<std::string>(), "启动 Blender 安装 (文件夹名称或部分匹配)")
		("list-installed", "列出本地版本目录下已安装的 Blender 版本")
		("delete", po::value<std::string>(), "删除已安装的 Blender 版本 (文件夹名称或部分匹配) ");
	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	}
	catch (const std::exception& ex) {
		std::cerr << "命令行解析错误: " << ex.what() << "\n";
		std::cerr << desc << "\n";
		return 1;
	}
	if (vm.count("help")) {
		std::cout << desc << "\n";
		return 0;
	}

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
	{
		std::cerr << "无法初始化curl" << std::endl;
		return 1;
	}

	std::atexit(curl_global_cleanup_impl);

	// 确保工作区和加载配置
	ensure_workspace_structure();
	Config cfg = ConfigManager::load();

	// 操作选项（不含 --help 和 --name，--name 可作为修饰符与其他选项组合）
	const std::vector<std::pair<std::string, std::function<void()>>> actions = {
		{"config", [&]() { handle_config(vm, cfg); }},
		{"list-installed", [&]() { handle_list_installed(vm); }},
		{"delete", [&]() { handle_delete(vm); }},
		{"download", [&]() { handle_download(vm, cfg); }},
		{"start", [&]() { handle_start(vm); }},
		{"show-all-ver", [&]() { handle_show_all_ver(); }},
		{"show-small-ver", [&]() { handle_show_small_ver(vm); }}
	};

	// 主操作选项列表（不包含 --name，它可作为修饰符与其他选项组合）
	std::vector<std::string> chosen_main;
	for (const auto& [name, _] : actions) {
		if (vm.count(name)) chosen_main.push_back("--" + name);
	}

	if (chosen_main.size() > 1) {
		std::cerr << "错误：操作选项不可组合使用，一次只能指定一个。\n"
				  << "检测到 " << chosen_main.size() << " 个操作选项: ";
		for (size_t i = 0; i < chosen_main.size(); ++i)
			std::cerr << (i ? ", " : "") << chosen_main[i];
		std::cerr << "\n使用 --help 查看帮助信息。\n";
		return 1;
	}

	// 如果同时指定了 --name 和其他选项，优先执行其他选项，--name 作为修饰符由对应 handler 自行处理
	if (chosen_main.size() == 1) {
		for (const auto& [name, handler] : actions) {
			if (vm.count(name)) { handler(); break; }
		}
	} else if (vm.count("name")) {
		// 只有 --name 一个选项时，独立执行
		handle_name(vm);
	} else {
		// 未指定任何操作，打印帮助信息
		std::cout << desc << "\n";
	}
	return 0;
}