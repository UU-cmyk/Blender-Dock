#pragma once
#include <string>
#include <filesystem>
#include <boost/predef/os.h>

// 返回可执行文件所在目录（平台特定实现）
std::filesystem::path get_executable_dir();

// 获取当前操作系统名称
std::string get_system_name();

// 输出系统信息（CPU 特性等）
void print_sys_info();

// 确保工作区文件夹结构存在
void ensure_workspace_structure();

// 读取文件全部内容为字符串
std::string read_file_all(const std::filesystem::path& p);

#if BOOST_OS_WINDOWS
// Windows: 构建可传递给 CreateProcess 的命令行字符串
std::string build_windows_command_line(const std::vector<std::string>& args);
#endif
