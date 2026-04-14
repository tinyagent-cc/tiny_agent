#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/skills/skill.hpp>
#include <tiny_agent/skills/loader.hpp>
#include <tiny_agent/skills/registry.hpp>
#include <filesystem>
#include <fstream>

using namespace tiny_agent::skills;

// ═══════════════════════════════════════════════════════════════════════════
// Frontmatter parsing
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("parse_frontmatter: basic key-value") {
    std::string content = R"(---
name: my-skill
description: Does cool things
---

# Instructions
Do the thing.)";

    auto [meta, body] = SkillLoader::parse_frontmatter(content);

    CHECK(meta["name"]        == "my-skill");
    CHECK(meta["description"] == "Does cool things");
    CHECK(body.find("# Instructions") != std::string::npos);
    CHECK(body.find("Do the thing.") != std::string::npos);
}

TEST_CASE("parse_frontmatter: quoted values") {
    std::string content = R"(---
name: "quoted-skill"
description: 'single quoted'
---
body)";

    auto [meta, body] = SkillLoader::parse_frontmatter(content);
    CHECK(meta["name"]        == "quoted-skill");
    CHECK(meta["description"] == "single quoted");
    CHECK(body == "body");
}

TEST_CASE("parse_frontmatter: extra fields") {
    std::string content = R"(---
name: skill
description: desc
author: Alice
version: 1.0
---
content)";

    auto [meta, body] = SkillLoader::parse_frontmatter(content);
    CHECK(meta.size() == 4);
    CHECK(meta["author"]  == "Alice");
    CHECK(meta["version"] == "1.0");
}

TEST_CASE("parse_frontmatter: no frontmatter") {
    std::string content = "Just plain markdown\n# Heading";
    auto [meta, body] = SkillLoader::parse_frontmatter(content);
    CHECK(meta.empty());
    CHECK(body == content);
}

TEST_CASE("parse_frontmatter: empty content") {
    auto [meta, body] = SkillLoader::parse_frontmatter("");
    CHECK(meta.empty());
    CHECK(body.empty());
}

TEST_CASE("parse_frontmatter: unclosed frontmatter") {
    std::string content = "---\nname: broken\nno closing";
    auto [meta, body] = SkillLoader::parse_frontmatter(content);
    CHECK(meta.empty());
    CHECK(body == content);
}

// ═══════════════════════════════════════════════════════════════════════════
// Skill struct
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Skill validity") {
    Skill s;
    CHECK_FALSE(s.valid());

    s.name = "test";
    CHECK_FALSE(s.valid());

    s.description = "desc";
    CHECK(s.valid());
}

TEST_CASE("Skill to_prompt_section") {
    Skill s{.name = "review", .description = "Review code",
            .instructions = "Check for bugs.\n"};

    auto section = s.to_prompt_section();
    CHECK(section.find("<skill name=\"review\">") != std::string::npos);
    CHECK(section.find("Review code") != std::string::npos);
    CHECK(section.find("Check for bugs.") != std::string::npos);
    CHECK(section.find("</skill>") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// SkillLoader from filesystem
// ═══════════════════════════════════════════════════════════════════════════

class TempSkillDir {
    std::filesystem::path dir_;
public:
    TempSkillDir() {
        dir_ = std::filesystem::temp_directory_path() / "tiny_agent_test_skills";
        std::filesystem::create_directories(dir_ / "alpha");
        std::filesystem::create_directories(dir_ / "beta");

        write(dir_ / "alpha" / "SKILL.md", R"(---
name: alpha
description: Alpha skill for testing
---

# Alpha
Do alpha things.)");

        write(dir_ / "beta" / "SKILL.md", R"(---
name: beta
description: Beta skill for testing
---

# Beta
Do beta things.)");
    }

    ~TempSkillDir() {
        std::filesystem::remove_all(dir_);
    }

    std::string path() const { return dir_.string(); }

private:
    static void write(const std::filesystem::path& p, const std::string& content) {
        std::ofstream f(p);
        f << content;
    }
};

TEST_CASE("SkillLoader::load single file") {
    TempSkillDir tmp;
    auto skill = SkillLoader::load(tmp.path() + "/alpha/SKILL.md");

    CHECK(skill.name == "alpha");
    CHECK(skill.description == "Alpha skill for testing");
    CHECK(skill.instructions.find("Do alpha things.") != std::string::npos);
    CHECK(skill.valid());
}

TEST_CASE("SkillLoader::discover finds all skills") {
    TempSkillDir tmp;
    auto skills = SkillLoader::discover(tmp.path());

    CHECK(skills.size() == 2);
    CHECK(skills[0].name == "alpha");
    CHECK(skills[1].name == "beta");
}

TEST_CASE("SkillLoader::discover empty directory") {
    auto tmp = std::filesystem::temp_directory_path() / "tiny_agent_empty_skills";
    std::filesystem::create_directories(tmp);
    auto skills = SkillLoader::discover(tmp.string());
    CHECK(skills.empty());
    std::filesystem::remove_all(tmp);
}

TEST_CASE("SkillLoader::discover non-existent directory") {
    auto skills = SkillLoader::discover("/non/existent/path");
    CHECK(skills.empty());
}

TEST_CASE("SkillLoader::load non-existent file throws") {
    CHECK_THROWS(SkillLoader::load("/no/such/SKILL.md"));
}

TEST_CASE("SkillLoader fallback name from directory") {
    auto dir = std::filesystem::temp_directory_path() / "tiny_agent_noname";
    std::filesystem::create_directories(dir / "my-dir");
    {
        std::ofstream f(dir / "my-dir" / "SKILL.md");
        f << "---\ndescription: no name field\n---\ncontent";
    }

    auto skill = SkillLoader::load((dir / "my-dir" / "SKILL.md").string());
    CHECK(skill.name == "my-dir");
    std::filesystem::remove_all(dir);
}

// ═══════════════════════════════════════════════════════════════════════════
// SkillRegistry
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SkillRegistry: add and get") {
    SkillRegistry reg;
    reg.add(Skill{.name = "s1", .description = "d1", .instructions = "i1"});

    CHECK(reg.size() == 1);
    CHECK(reg.has("s1"));
    CHECK_FALSE(reg.has("s2"));
    CHECK(reg.get("s1").description == "d1");
}

TEST_CASE("SkillRegistry: get unknown throws") {
    SkillRegistry reg;
    CHECK_THROWS_AS(reg.get("nope"), std::runtime_error);
}

TEST_CASE("SkillRegistry: list is sorted") {
    SkillRegistry reg;
    reg.add(Skill{.name = "zebra", .description = "z"});
    reg.add(Skill{.name = "alpha", .description = "a"});
    reg.add(Skill{.name = "middle", .description = "m"});

    auto names = reg.list();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "alpha");
    CHECK(names[1] == "middle");
    CHECK(names[2] == "zebra");
}

TEST_CASE("SkillRegistry: add_from_directory") {
    TempSkillDir tmp;
    SkillRegistry reg;
    reg.add_from_directory(tmp.path());

    CHECK(reg.size() == 2);
    CHECK(reg.has("alpha"));
    CHECK(reg.has("beta"));
}

TEST_CASE("SkillRegistry: build_prompt selected") {
    SkillRegistry reg;
    reg.add(Skill{.name = "a", .description = "da", .instructions = "Do A.\n"});
    reg.add(Skill{.name = "b", .description = "db", .instructions = "Do B.\n"});
    reg.add(Skill{.name = "c", .description = "dc", .instructions = "Do C.\n"});

    auto prompt = reg.build_prompt({"a", "c"});
    CHECK(prompt.find("<skill name=\"a\">") != std::string::npos);
    CHECK(prompt.find("<skill name=\"c\">") != std::string::npos);
    CHECK(prompt.find("<skill name=\"b\">") == std::string::npos);
}

TEST_CASE("SkillRegistry: build_prompt all") {
    SkillRegistry reg;
    reg.add(Skill{.name = "x", .description = "dx", .instructions = "X\n"});
    reg.add(Skill{.name = "y", .description = "dy", .instructions = "Y\n"});

    auto prompt = reg.build_prompt();
    CHECK(prompt.find("<skill name=\"x\">") != std::string::npos);
    CHECK(prompt.find("<skill name=\"y\">") != std::string::npos);
}

TEST_CASE("SkillRegistry: empty build_prompt") {
    SkillRegistry reg;
    CHECK(reg.build_prompt().empty());
}
