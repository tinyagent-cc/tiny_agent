#pragma once
#include <string>
#include <map>

namespace tiny_agent::skills {

// ── Skill — a loaded agent skill (from a SKILL.md file) ─────────────────────
//
// Follows the Agent Skills specification (agentskills.io).
// A skill is a folder containing a SKILL.md with YAML frontmatter:
//
//   ---
//   name: my-skill
//   description: What this skill does and when to use it.
//   ---
//
//   # Instructions
//   ...

struct Skill {
    std::string name;
    std::string description;
    std::string instructions;     // markdown body below frontmatter
    std::string path;             // filesystem path to the SKILL.md

    std::map<std::string, std::string> metadata;   // extra frontmatter fields

    [[nodiscard]] bool valid() const {
        return !name.empty() && !description.empty();
    }

    // Format instructions for injection into a system prompt.
    [[nodiscard]] std::string to_prompt_section() const {
        std::string section = "<skill name=\"" + name + "\">\n";
        if (!description.empty())
            section += "<!-- " + description + " -->\n";
        section += instructions;
        if (!section.empty() && section.back() != '\n')
            section += '\n';
        section += "</skill>";
        return section;
    }
};

} // namespace tiny_agent::skills
