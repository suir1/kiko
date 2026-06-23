#include "transfer/gitignore.hpp"

#include <fstream>

namespace kiko {
namespace {

bool fnmatch(const std::string& pattern, const std::string& text) {
  std::size_t pi = 0;
  std::size_t ti = 0;
  std::size_t star_pi = std::string::npos;
  std::size_t star_ti = 0;

  while (ti < text.size()) {
    if (pi < pattern.size() && (pattern[pi] == text[ti] || pattern[pi] == '?')) {
      ++pi;
      ++ti;
      continue;
    }
    if (pi < pattern.size() && pattern[pi] == '*') {
      star_pi = ++pi;
      star_ti = ti;
      continue;
    }
    if (star_pi != std::string::npos) {
      pi = star_pi;
      ti = ++star_ti;
      continue;
    }
    return false;
  }
  while (pi < pattern.size() && pattern[pi] == '*') ++pi;
  return pi == pattern.size();
}

}  // namespace

bool GitIgnore::match_rule(const Rule& rule, const std::string& relative_path) {
  if (rule.directory && relative_path.find('/') == std::string::npos) return false;
  if (rule.pattern.find('/') == std::string::npos) {
    const auto slash = relative_path.find_last_of('/');
    const auto base = slash == std::string::npos ? relative_path : relative_path.substr(slash + 1);
    return fnmatch(rule.pattern, base) || fnmatch(rule.pattern, relative_path);
  }
  return fnmatch(rule.pattern, relative_path);
}

void GitIgnore::add_line(const std::string& line) {
  auto trimmed = line;
  while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
  std::size_t start = 0;
  while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\t')) ++start;
  trimmed = trimmed.substr(start);
  if (trimmed.empty() || trimmed.front() == '#') return;

  Rule rule;
  if (trimmed.front() == '!') {
    rule.negated = true;
    trimmed = trimmed.substr(1);
  }
  if (!trimmed.empty() && trimmed.back() == '/') {
    rule.directory = true;
    trimmed.pop_back();
  }
  rule.pattern = trimmed;
  if (!rule.pattern.empty()) rules_.push_back(rule);
}

void GitIgnore::add_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) add_line(line);
}

bool GitIgnore::ignored(const std::string& relative_path) const {
  bool ignored = false;
  for (const auto& rule : rules_) {
    if (match_rule(rule, relative_path)) ignored = !rule.negated;
  }
  return ignored;
}

GitIgnore load_gitignore_stack(const std::filesystem::path& root) {
  GitIgnore stack;
  std::error_code ec;
  for (auto dir = std::filesystem::absolute(root, ec); !ec; dir = dir.parent_path()) {
    const auto gi = dir / ".gitignore";
    if (std::filesystem::is_regular_file(gi, ec)) stack.add_file(gi);
    if (!dir.has_parent_path() || dir == dir.parent_path()) break;
  }
  return stack;
}

}  // namespace kiko
