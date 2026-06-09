#include "config_manager.h"
#include <fstream>
#include <filesystem>
#include <iostream>

using json = nlohmann::json;

Config ConfigManager::load(const std::string& path)
{
	Config cfg;
	std::filesystem::path p(path);
	if (!std::filesystem::exists(p)) {
		// ensure directory exists
		try { std::filesystem::create_directories(p.parent_path()); }
		catch (...) {}
		// write defaults
		save(cfg, path);
		return cfg;
	}

	std::ifstream ifs(path);
	if (!ifs) return cfg;
	try {
		json j; ifs >> j;
		if (j.contains("parallel_downloads") && j["parallel_downloads"].is_number_integer()) cfg.parallel = static_cast<unsigned int>(j["parallel_downloads"].get<int>());
		if (j.contains("auto_extract_addons")) cfg.auto_extract_addons = j["auto_extract_addons"].get<bool>();
		if (j.contains("auto_link_addons")) cfg.auto_link_addons = j["auto_link_addons"].get<bool>();
		if (j.contains("blender_user_dir") && j["blender_user_dir"].is_string()) cfg.blender_user_dir = j["blender_user_dir"].get<std::string>();
	}
	catch (...) { }
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
		std::filesystem::path p(path);
		std::string s;
		if (std::filesystem::exists(p)) {
			std::ifstream ifs(path);
			if (ifs) { std::ostringstream ss; ss << ifs.rdbuf(); s = ss.str(); }
		}
		if (s.empty()) s = "{}";

		json j = json::parse(s);
		j[key] = value;

		std::ofstream ofs(path, std::ios::trunc);
		if (!ofs) return;
		ofs << j.dump(2) << std::endl;
	}
	catch (...) { }
}
