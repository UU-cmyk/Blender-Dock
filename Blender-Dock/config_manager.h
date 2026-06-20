#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// 配置结构体：包含所有可配置的选项
struct Config {
	unsigned int parallel = 0;
	bool auto_extract_addons = true;
	bool auto_link_addons = false;
	std::string blender_user_dir;
};

// 配置管理器：负责加载、保存和更新配置
struct ConfigManager {
	static std::optional<json> readJsonFile(const std::string& path); // 读取JSON文件并返回json对象
	static Config load(const std::string& path = "config/config.json"); // 从指定路径加载配置
	static void save(const Config& cfg, const std::string& path = "config/config.json"); // 将配置保存到指定路径
	static void updateField(const std::string& path, const std::string& key, int value); // 更新整数字段
};
