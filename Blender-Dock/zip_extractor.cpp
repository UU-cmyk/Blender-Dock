#include "zip_extractor.h"
#include <zip.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <fstream>

bool ZipExtractor::extractTo(const std::filesystem::path& archive, const std::filesystem::path& destDir)
{
	try {
		std::filesystem::create_directories(destDir);
	}
	catch (...) { return false; }

	int err = 0;
	zip_t* za = zip_open(archive.string().c_str(), ZIP_RDONLY, &err);
	if (!za) { std::cerr << "libzip: failed to open archive: " << archive.string() << " (err=" << err << ")\n"; return false; }
	struct ZipCloser { zip_t* za; ZipCloser(zip_t* p) : za(p) {} ~ZipCloser() { if (za) zip_close(za); } } zguard(za);

	zip_int64_t n = zip_get_num_entries(za, 0);
	for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(n); ++i) {
		struct zip_stat st;
		if (zip_stat_index(za, i, 0, &st) != 0) continue;
		std::string name = st.name;
		std::filesystem::path entry = std::filesystem::path(name);
		std::filesystem::path outpath = destDir / entry;

		if (!name.empty() && (name.back() == '/' || name.back() == '\\')) {
			try { std::filesystem::create_directories(outpath); }
			catch (...) {}
			continue;
		}

		if (outpath.has_parent_path()) {
			try { std::filesystem::create_directories(outpath.parent_path()); }
			catch (...) {}
		}

		zip_file_t* zf = zip_fopen_index(za, i, 0);
		if (!zf) { std::cerr << "libzip: failed to open file in archive: " << name << "\n"; return false; }
		struct ZipFileCloser { zip_file_t* zf; ZipFileCloser(zip_file_t* p) : zf(p) {} ~ZipFileCloser() { if (zf) zip_fclose(zf); } } zfguard(zf);

		std::ofstream ofs(outpath, std::ios::binary);
		if (!ofs) { std::cerr << "Failed to create output file: " << outpath.string() << "\n"; return false; }

		const size_t BUF_SIZE = 8192;
		std::vector<char> buffer(BUF_SIZE);
		zip_int64_t bytesRead = 0;
		while ((bytesRead = zip_fread(zf, buffer.data(), static_cast<zip_uint64_t>(buffer.size()))) > 0) {
			ofs.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
			if (!ofs) { std::cerr << "Write failed to: " << outpath.string() << "\n"; return false; }
		}
		ofs.close();
	}
	return true;
}

bool ZipExtractor::extractAndOrganize(const std::filesystem::path& archive, const std::filesystem::path& targetDir, const std::string& targetName)
{
	std::filesystem::path tmp = targetDir / (archive.filename().string() + std::string(".tmp_extract"));
	try { std::filesystem::remove_all(tmp); }
	catch (...) {}

	bool ok = extractTo(archive, tmp);
	if (!ok) return false;

	std::vector<std::filesystem::path> children;
	for (auto& e : std::filesystem::directory_iterator(tmp)) children.push_back(e.path());

	std::filesystem::path targetPath = targetDir / targetName;
	try {
		if (std::filesystem::exists(targetPath)) std::filesystem::remove_all(targetPath);

		if (children.size() == 1 && std::filesystem::is_directory(children[0])) {
			std::filesystem::rename(children[0], targetPath);
		}
		else {
			std::filesystem::create_directories(targetPath);
			for (auto& p : children) {
				std::filesystem::path dest = targetPath / p.filename();
				try { std::filesystem::rename(p, dest); }
				catch (...) {
					std::filesystem::copy(p, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
					std::filesystem::remove_all(p);
				}
			}
		}
	}
	catch (const std::exception& ex) {
		std::cerr << "Failed to move extracted files: " << ex.what() << "\n";
		try { std::filesystem::remove_all(tmp); } catch (...) {}
		return false;
	}

	try { std::filesystem::remove_all(tmp); }
	catch (...) {}
	try { std::filesystem::remove(archive); } catch (...) {}
	return true;
}
