#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kiko {

class GitIgnore {
 public:
  void add_file(const std::filesystem::path& path);
  void add_line(const std::string& line);
  [[nodiscard]] bool ignored(const std::string& relative_path) const;

 private:
  struct Rule {
    std::string pattern;
    bool directory = false;
    bool negated = false;
  };
  static bool match_rule(const Rule& rule, const std::string& relative_path);
  std::vector<Rule> rules_;
};

[[nodiscard]] GitIgnore load_gitignore_stack(const std::filesystem::path& root);

}  // namespace kiko
