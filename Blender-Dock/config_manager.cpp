#include "config_manager.h"
#include <fstream>
#include <filesystem>
#include <iostream>

using json = nlohmann::json;

std::optional<json> ConfigManager::readJsonFile(const std::string& path) {
	std::ifstream ifs(path);
	if (!ifs) return std::nullopt;
	try { return json::parse(ifs); }
	catch (...) { return std::nullopt; }
}

Config ConfigManager::load(const std::string& path)
{
	Config cfg;
	std::filesystem::path p(path);
	if (!std::filesystem::exists(p)) {
		// 确保目录存在
		try { std::filesystem::create_directories(p.parent_path()); }
		catch (...) {}
		save(cfg, path); // 写入默认值
		return cfg;
	}

	auto opt = readJsonFile(path);
	if (!opt) return cfg;
	json j = *opt;
	if (j.contains("parallel_downloads") && j["parallel_downloads"].is_number_integer()) cfg.parallel = static_cast<unsigned int>(j["parallel_downloads"].get<int>());
	if (j.contains("auto_extract_addons")) cfg.auto_extract_addons = j["auto_extract_addons"].get<bool>();
	if (j.contains("auto_link_addons")) cfg.auto_link_addons = j["auto_link_addons"].get<bool>();
	if (j.contains("blender_user_dir") && j["blender_user_dir"].is_string()) cfg.blender_user_dir = j["blender_user_dir"].get<std::string>();
	return cfg;
}

void ConfigManager::save(const Config& cfg, const std::string& path)
{
	try {
		std::filesystem::path p(path);
		std::filesystem::create_directories(p.parent_path());
		json j;
		j["parallel_downloads"] = cfg.parallel;
		j["auto_extract_addons"] = cfg.auto_extract_addons;
		j["auto_link_addons"] = cfg.auto_link_addons;
		j["blender_user_dir"] = cfg.blender_user_dir;
		std::ofstream ofs(path, std::ios::trunc);
		if (!ofs) return;
		ofs << j.dump(2) << std::endl;
	}
	catch (...) { }
}

void ConfigManager::updateField(const std::string& path, const std::string& key, int value)
{
	try {
		json j;
		auto opt = readJsonFile(path);
		if (opt) j = *opt;
		else j = json::object();
		j[key] = value; // 更新指定字段

		std::ofstream ofs(path, std::ios::trunc);
		if (!ofs) return;
		ofs << j.dump(2) << std::endl;
	}
	catch (...) { }
}
