#pragma once
#include <boost/program_options.hpp>

// 前向声明 Config 以避免在头文件中依赖 config_manager.h 的具体路径
// 这能在项目没有正确包含目录时减少编译错误。
struct Config;

namespace po = boost::program_options;

// 配置设置：例如 --config download parallel_downloads=4
void handle_config(const po::variables_map& vm, Config& cfg);

// 列出本地版本目录下已安装的 Blender 版本
void handle_list_installed(const po::variables_map& vm);

// 下载指定文件（可为文件名或完整 URL）
void handle_download(const po::variables_map& vm, Config& cfg);

// 启动指定已解压的 Blender 目录
void handle_start(const po::variables_map& vm);

// 列出所有大版本
void handle_show_all_ver();

// 列出指定大版本下可用的小版本（过滤当前系统）
void handle_show_small_ver(const po::variables_map& vm);

// 删除已安装的 Blender 版本（文件夹名称或部分匹配）
void handle_delete(const po::variables_map& vm);

// 处理 --name 选项（设置自定义名称）
void handle_name(const po::variables_map& vm);
