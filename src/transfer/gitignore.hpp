#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kiko {

class GitIgnore {
 public:
  void add_file(const std::filesystem::path& path);
  void add_line(const std::string& line);
  [[nodiscard]] bool ignored(const std::filesystem::path& path, bool is_directory = false) const;

 private:
  struct Rule {
    std::filesystem::path base;
    std::string pattern;
    bool directory_only = false;
    bool negated = false;
    bool anchored = false;
  };
  void add_line(const std::string& line, const std::filesystem::path& base);
  static bool match_rule(const Rule& rule, const std::filesystem::path& path, bool is_directory);
  std::vector<Rule> rules_;
};

[[nodiscard]] GitIgnore load_gitignore_stack(const std::filesystem::path& root);

}  // namespace kiko
