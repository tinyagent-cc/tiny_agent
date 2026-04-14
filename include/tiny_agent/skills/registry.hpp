#pragma once
#include "skill.hpp"
#include "loader.hpp"
#include <unordered_map>
#include <stdexcept>

namespace tiny_agent::skills {

// ── SkillRegistry — manage a collection of loaded skills ─────────────────────

class SkillRegistry {
    std::unordered_map<std::string, Skill> skills_;

public:
    SkillRegistry() = default;

    // Add a single skill.
    void add(Skill skill) {
        skills_[skill.name] = std::move(skill);
    }

    // Load and add a single SKILL.md from a file path.
    void add_from_file(const std::string& path) {
        add(SkillLoader::load(path));
    }

    // Discover and add all skills from a directory tree.
    void add_from_directory(const std::string& dir) {
        for (auto& s : SkillLoader::discover(dir))
            add(std::move(s));
    }

    // Retrieve a skill by name.
    [[nodiscard]] const Skill& get(const std::string& name) const {
        auto it = skills_.find(name);
        if (it == skills_.end())
            throw std::runtime_error("Unknown skill: " + name);
        return it->second;
    }

    [[nodiscard]] bool has(const std::string& name) const {
        return skills_.count(name) > 0;
    }

    [[nodiscard]] std::vector<std::string> list() const {
        std::vector<std::string> names;
        names.reserve(skills_.size());
        for (auto& [k, _] : skills_) names.push_back(k);
        std::sort(names.begin(), names.end());
        return names;
    }

    [[nodiscard]] std::size_t size() const { return skills_.size(); }
    [[nodiscard]] bool empty() const { return skills_.empty(); }

    auto begin() const { return skills_.begin(); }
    auto end()   const { return skills_.end(); }

    // Build a system-prompt section from selected skills.
    [[nodiscard]] std::string build_prompt(
        const std::vector<std::string>& skill_names) const
    {
        std::string prompt;
        for (auto& name : skill_names) {
            auto it = skills_.find(name);
            if (it == skills_.end()) continue;
            if (!prompt.empty()) prompt += "\n\n";
            prompt += it->second.to_prompt_section();
        }
        return prompt;
    }

    // Build a system-prompt section from ALL loaded skills.
    [[nodiscard]] std::string build_prompt() const {
        auto names = list();
        return build_prompt(names);
    }
};

} // namespace tiny_agent::skills
