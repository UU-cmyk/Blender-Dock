#pragma once
#include <string>
#include <nlohmann/json.hpp>

struct Config {
	unsigned int parallel = 0;
	bool auto_extract_addons = true;
	bool auto_link_addons = false;
	std::string blender_user_dir;
};

struct ConfigManager {
	static Config load(const std::string& path = "config/config.json");
	static void save(const Config& cfg, const std::string& path = "config/config.json");
	static void updateField(const std::string& path, const std::string& key, int value);
};
