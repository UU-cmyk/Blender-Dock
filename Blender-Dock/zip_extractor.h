#pragma once
#include <filesystem>
#include <string>

struct ZipExtractor {
	// Extract archive into destDir (creates destDir if needed)
	static bool extractTo(const std::filesystem::path& archive, const std::filesystem::path& destDir);

	// Extract archive into a temporary location and organize into targetDir/targetName
	// If archive contains a single top-level directory, it will be renamed to targetName.
	// Otherwise, all top-level entries are moved into targetDir/targetName.
	static bool extractAndOrganize(const std::filesystem::path& archive, const std::filesystem::path& targetDir, const std::string& targetName);
};
