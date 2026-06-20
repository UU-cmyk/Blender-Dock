#include "version_parser.h"
#include <gumbo.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <set>
#include <sstream>
#include <vector>
#include <boost/predef/os.h>
#if BOOST_OS_WINDOWS
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

static void parse_major_versions_node(GumboNode* node, std::vector<std::string>& out)
{
	if (node->type != GUMBO_NODE_ELEMENT) return;
	if (node->v.element.tag == GUMBO_TAG_A) {
		GumboAttribute* href = gumbo_get_attribute(&node->v.element.attributes, "href");
		if (href) {
			std::string s = href->value;
			if (s.find("Blender") == 0 && s.back() == '/') {
				s.pop_back();
				auto tolower_copy = [](const std::string& str) { std::string t; t.reserve(str.size()); for (char c : str) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); return t; };
				std::string low = tolower_copy(s);
				bool exclude = false;
				if (low.find("blenderbenchmark") != std::string::npos) exclude = true;
				if (low.find("alpha") != std::string::npos) exclude = true;
				if (low.find("beta") != std::string::npos) exclude = true;
				if (low.find("newpy") != std::string::npos) exclude = true;
				if (!exclude && !s.empty()) {
					char last = s.back();
					if ((last == 'a' || last == 'A' || last == 'b' || last == 'B' || last == 'c' || last == 'C') && s.size() >= 2 && std::isdigit(static_cast<unsigned char>(s[s.size() - 2]))) {
						exclude = true;
					}
				}
				if (!exclude) out.push_back(s);
			}
		}
	}

	auto* c = &node->v.element.children;
	for (unsigned i = 0; i < c->length; ++i) parse_major_versions_node(static_cast<GumboNode*>(c->data[i]), out);
}

static void parse_download_links_node(GumboNode* node, std::set<std::string>& out)
{
	if (node->type != GUMBO_NODE_ELEMENT) return;
	if (node->v.element.tag == GUMBO_TAG_A) {
		GumboAttribute* href = gumbo_get_attribute(&node->v.element.attributes, "href");
		if (href) {
			std::string s = href->value;
			if (s.ends_with(".zip") || s.ends_with(".tar.xz") || s.ends_with(".dmg")) {
				out.insert(s);
			}
		}
	}

	auto* c = &node->v.element.children;
	for (unsigned i = 0; i < c->length; ++i) parse_download_links_node(static_cast<GumboNode*>(c->data[i]), out);
}

std::vector<std::string> VersionParser::parseMajorVersions(const std::string& html) const
{
	std::vector<std::string> out;
	GumboOutput* root = gumbo_parse(html.c_str());
	parse_major_versions_node(root->root, out);
	gumbo_destroy_output(&kGumboDefaultOptions, root);
	return out;
}

std::set<std::string> VersionParser::parseDownloadLinks(const std::string& html) const
{
	std::set<std::string> out;
	GumboOutput* root = gumbo_parse(html.c_str());
	parse_download_links_node(root->root, out);
	gumbo_destroy_output(&kGumboDefaultOptions, root);
	return out;
}

static bool filename_matches_arch(const std::string& name_lower)
{
	// 此函数供 filterBySystem() 使用
	// 在文件名中检测架构相关提示
	// 注意：filterBySystem() 会决定实际架构，这里只是简单检查文件名
	return true; // 占位；实际逻辑在 filterBySystem() 中实现
}

std::vector<std::string> VersionParser::filterBySystem(const std::set<std::string>& files) const
{
	std::vector<std::string> available;

	auto get_arch = []() -> std::string {
#if BOOST_OS_WINDOWS
		SYSTEM_INFO si;
		GetNativeSystemInfo(&si);
		switch (si.wProcessorArchitecture) {
		case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
		case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
		case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
		case PROCESSOR_ARCHITECTURE_ARM: return "arm";
		default: return "unknown";
		}
#else
		struct utsname u;
		if (uname(&u) != 0) return "unknown";
		std::string m(u.machine);
		std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (m.find("aarch64") != std::string::npos || m.find("arm64") != std::string::npos) return "arm64";
		if (m.find("armv7") != std::string::npos || m.find("armv6") != std::string::npos || m.find("arm") != std::string::npos) return "arm";
		if (m.find("x86_64") != std::string::npos || m.find("amd64") != std::string::npos) return "x86_64";
		if (m.find("86") != std::string::npos || m.find("i386") != std::string::npos) return "x86";
		return "unknown";
#endif
	};

	auto arch = get_arch();

	auto filename_matches_arch2 = [&](const std::string& name_lower) -> bool {
		// 根据文件名中的关键词推断是否匹配当前系统架构
		if (name_lower.find("arm64") != std::string::npos || name_lower.find("aarch64") != std::string::npos) {
			return arch == "arm64";
		}
		if (name_lower.find("armv7") != std::string::npos || name_lower.find("armv6") != std::string::npos || name_lower.find("armhf") != std::string::npos || name_lower.find("arm") != std::string::npos) {
			return arch == "arm" || arch == "arm64";
		}
		if (name_lower.find("win64") != std::string::npos || name_lower.find("linux64") != std::string::npos || name_lower.find("x86_64") != std::string::npos || name_lower.find("amd64") != std::string::npos) {
			return arch == "x86_64" || arch == "arm64";
		}
		if (name_lower.find("win32") != std::string::npos || name_lower.find("i386") != std::string::npos || name_lower.find("x86") != std::string::npos) {
			return arch == "x86" || arch == "x86_64";
		}
		return true;
	};

	for (const auto& f : files) {
		std::string lower = f;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

#if defined(BOOST_OS_WINDOWS) || defined(BOOST_OS_LINUX)
		if (lower.find("osx") != std::string::npos) continue;
#endif
		{
			if (lower.find("rc") != std::string::npos || lower.find("preview") != std::string::npos || lower.find("alpha") != std::string::npos || lower.find("beta") != std::string::npos || lower.find("add-ons-legacy-bundle") != std::string::npos) continue;
			bool foundSuffix = false;
			for (size_t i = 1; i < lower.size(); ++i) {
				if (std::isdigit(static_cast<unsigned char>(lower[i - 1]))) {
					char c = lower[i];
					if (c == 'a' || c == 'b' || c == 'c') { foundSuffix = true; break; }
				}
			}
			if (foundSuffix) continue;
		}
		if (!filename_matches_arch2(lower)) continue;

#if BOOST_OS_WINDOWS
		if (f.ends_with(".zip")) available.push_back(f);
#endif
#if BOOST_OS_LINUX
		if (f.ends_with(".tar.xz") || f.ends_with(".zip")) available.push_back(f);
#endif
#if BOOST_OS_MACOS
		if (f.ends_with(".dmg") || f.ends_with(".zip")) available.push_back(f);
#endif
	}

	return available;
}

std::vector<int> parse_version_numbers(const std::string& s)
{
	std::vector<int> parts;
	size_t pos = s.find_first_of("0123456789");
	if (pos == std::string::npos) return parts;
	std::string sub = s.substr(pos);
	std::string cur;
	for (char c : sub) {
		if (std::isdigit(static_cast<unsigned char>(c))) cur.push_back(c);
		else if (c == '.') {
			if (!cur.empty()) { parts.push_back(std::stoi(cur)); cur.clear(); }
			else parts.push_back(0);
		}
		else break;
	}
	if (!cur.empty()) parts.push_back(std::stoi(cur));
	return parts;
}

bool filename_version_less(const std::string& a, const std::string& b)
{
	auto va = parse_version_numbers(a);
	auto vb = parse_version_numbers(b);
	size_t n = (va.size() > vb.size()) ? va.size() : vb.size();
	for (size_t i = 0; i < n; ++i) {
		int ai = i < va.size() ? va[i] : 0;
		int bi = i < vb.size() ? vb[i] : 0;
		if (ai < bi) return true; // 版本号部分比较，较小的版本号优先
		if (ai > bi) return false; // 版本号部分比较，较大的版本号优先
	}
	return a < b;
}
