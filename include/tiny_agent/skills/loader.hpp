#pragma once
#include "skill.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace tiny_agent::skills {

// ── SkillLoader — parse SKILL.md files and discover skill directories ───────

class SkillLoader {
public:

    // Parse YAML-style frontmatter from a SKILL.md string.
    // Returns { key→value map , markdown body }.
    static std::pair<std::map<std::string, std::string>, std::string>
    parse_frontmatter(const std::string& content) {
        std::map<std::string, std::string> meta;
        std::string body;

        if (content.size() < 3 || content.substr(0, 3) != "---") {
            return {meta, content};
        }

        auto end_marker = content.find("\n---", 3);
        if (end_marker == std::string::npos) {
            return {meta, content};
        }

        // Extract frontmatter block (between first --- and second ---)
        auto fm_start = content.find('\n', 0);
        if (fm_start == std::string::npos) return {meta, content};
        ++fm_start;

        std::string fm_block = content.substr(fm_start, end_marker - fm_start);

        // Parse key: value lines
        std::istringstream ss(fm_block);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);

            // Trim whitespace
            auto ltrim = [](std::string& s) {
                s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                    [](unsigned char c) { return !std::isspace(c); }));
            };
            auto rtrim = [](std::string& s) {
                s.erase(std::find_if(s.rbegin(), s.rend(),
                    [](unsigned char c) { return !std::isspace(c); }).base(),
                    s.end());
            };

            ltrim(key); rtrim(key);
            ltrim(val); rtrim(val);

            // Strip surrounding quotes
            if (val.size() >= 2 &&
                ((val.front() == '"' && val.back() == '"') ||
                 (val.front() == '\'' && val.back() == '\'')))
                val = val.substr(1, val.size() - 2);

            if (!key.empty()) meta[key] = val;
        }

        // Body is everything after the closing ---
        auto body_start = end_marker + 4;  // skip \n---
        if (body_start < content.size()) {
            body = content.substr(body_start);
            // Trim leading newlines from body
            while (!body.empty() && body.front() == '\n') body.erase(body.begin());
        }

        return {meta, body};
    }

    // Load a single SKILL.md from a filesystem path.
    static Skill load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open())
            throw std::runtime_error("Cannot open skill file: " + path);

        std::ostringstream ss;
        ss << file.rdbuf();

        auto [meta, body] = parse_frontmatter(ss.str());

        Skill skill;
        skill.path = path;
        skill.instructions = body;
        skill.metadata = meta;

        if (auto it = meta.find("name"); it != meta.end())
            skill.name = it->second;
        if (auto it = meta.find("description"); it != meta.end())
            skill.description = it->second;

        // Fall back to parent directory name if no name in frontmatter
        if (skill.name.empty()) {
            namespace fs = std::filesystem;
            skill.name = fs::path(path).parent_path().filename().string();
        }

        return skill;
    }

    // Recursively discover all SKILL.md files under a directory.
    static std::vector<Skill> discover(const std::string& root_dir) {
        namespace fs = std::filesystem;
        std::vector<Skill> skills;

        if (!fs::exists(root_dir) || !fs::is_directory(root_dir))
            return skills;

        for (auto& entry : fs::recursive_directory_iterator(root_dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().filename() != "SKILL.md") continue;

            try {
                skills.push_back(load(entry.path().string()));
            } catch (...) {
                // Skip malformed skill files
            }
        }

        std::sort(skills.begin(), skills.end(),
            [](const Skill& a, const Skill& b) { return a.name < b.name; });

        return skills;
    }
};

} // namespace tiny_agent::skills
