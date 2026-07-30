#include "transfer/gitignore.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <utility>

namespace kiko {
namespace {

bool character_class_matches(const std::string& pattern, std::size_t begin, std::size_t end, char value) {
  bool negated = false;
  if (begin < end && (pattern[begin] == '!' || pattern[begin] == '^')) {
    negated = true;
    ++begin;
  }

  bool matched = false;
  for (std::size_t i = begin; i < end;) {
    char first = pattern[i++];
    if (first == '\\' && i < end) first = pattern[i++];
    if (i + 1 < end && pattern[i] == '-') {
      ++i;
      char last = pattern[i++];
      if (last == '\\' && i < end) last = pattern[i++];
      if (first <= value && value <= last) matched = true;
    } else if (first == value) {
      matched = true;
    }
  }
  return negated ? !matched : matched;
}

bool wildmatch(const std::string& pattern, const std::string& text) {
  const auto columns = text.size() + 1;
  std::vector<std::int8_t> memo((pattern.size() + 1) * columns, -1);
  std::function<bool(std::size_t, std::size_t)> match = [&](std::size_t pi, std::size_t ti) {
    auto& cached = memo[pi * columns + ti];
    if (cached != -1) return cached != 0;

    bool result = false;
    if (pi == pattern.size()) {
      result = ti == text.size();
    } else if (pattern[pi] == '*') {
      std::size_t next = pi;
      while (next < pattern.size() && pattern[next] == '*') ++next;
      const bool globstar = next - pi >= 2 && (pi == 0 || pattern[pi - 1] == '/') &&
                            (next == pattern.size() || pattern[next] == '/');
      if (globstar) {
        if (next < pattern.size() && pattern[next] == '/' && match(next + 1, ti)) {
          result = true;
        } else if (match(next, ti)) {
          result = true;
        } else if (ti < text.size()) {
          result = match(pi, ti + 1);
        }
      } else {
        result = match(next, ti) || (ti < text.size() && text[ti] != '/' && match(pi, ti + 1));
      }
    } else if (ti < text.size() && pattern[pi] == '?') {
      result = text[ti] != '/' && match(pi + 1, ti + 1);
    } else if (ti < text.size() && pattern[pi] == '[') {
      const auto end = pattern.find(']', pi + 1);
      if (end == std::string::npos) {
        result = text[ti] == '[' && match(pi + 1, ti + 1);
      } else if (text[ti] != '/' && character_class_matches(pattern, pi + 1, end, text[ti])) {
        result = match(end + 1, ti + 1);
      }
    } else {
      std::size_t next = pi + 1;
      char expected = pattern[pi];
      if (expected == '\\' && next < pattern.size()) expected = pattern[next++];
      result = ti < text.size() && expected == text[ti] && match(next, ti + 1);
    }

    cached = result ? 1 : 0;
    return result;
  };
  return match(0, 0);
}

std::filesystem::path absolute_normalized(const std::filesystem::path& path) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  return ec ? path.lexically_normal() : absolute.lexically_normal();
}

std::optional<std::string> path_relative_to_rule(const std::filesystem::path& path,
                                                 const std::filesystem::path& base) {
  if (base.empty()) return path.generic_string();
  const auto relative = absolute_normalized(path).lexically_relative(base);
  if (relative.empty() || relative == ".") return std::nullopt;
  for (const auto& part : relative) {
    if (part == "..") return std::nullopt;
  }
  return relative.generic_string();
}

std::vector<std::pair<std::string, bool>> path_candidates(const std::string& path, bool is_directory) {
  std::vector<std::pair<std::string, bool>> candidates;
  std::size_t start = 0;
  std::string prefix;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto component = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!component.empty()) {
      if (!prefix.empty()) prefix += '/';
      prefix += component;
      candidates.emplace_back(prefix, slash != std::string::npos || is_directory);
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return candidates;
}

bool trailing_character_is_escaped(const std::string& value) {
  if (value.size() < 2) return false;
  std::size_t slashes = 0;
  for (std::size_t i = value.size() - 1; i > 0 && value[i - 1] == '\\'; --i) ++slashes;
  return slashes % 2 == 1;
}

void trim_pattern_line(std::string& line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t') && !trailing_character_is_escaped(line)) {
    line.pop_back();
  }
}

}  // namespace

bool GitIgnore::match_rule(const Rule& rule, const std::filesystem::path& path, bool is_directory) {
  const auto relative = path_relative_to_rule(path, rule.base);
  if (!relative) return false;
  const auto candidates = path_candidates(*relative, is_directory);
  const bool path_pattern = rule.anchored || rule.pattern.find('/') != std::string::npos;

  for (const auto& [candidate, candidate_is_directory] : candidates) {
    if (rule.directory_only && !candidate_is_directory) continue;
    if (path_pattern) {
      if (wildmatch(rule.pattern, candidate)) return true;
      continue;
    }
    const auto slash = candidate.find_last_of('/');
    const auto component = slash == std::string::npos ? candidate : candidate.substr(slash + 1);
    if (wildmatch(rule.pattern, component)) return true;
  }
  return false;
}

void GitIgnore::add_line(const std::string& line) {
  add_line(line, {});
}

void GitIgnore::add_line(const std::string& line, const std::filesystem::path& base) {
  auto pattern = line;
  trim_pattern_line(pattern);
  if (pattern.empty() || pattern.front() == '#') return;

  Rule rule;
  rule.base = base;
  if (pattern.front() == '!') {
    rule.negated = true;
    pattern.erase(0, 1);
  }
  if (!pattern.empty() && pattern.back() == '/' && !trailing_character_is_escaped(pattern)) {
    rule.directory_only = true;
    pattern.pop_back();
  }
  if (!pattern.empty() && pattern.front() == '/') {
    rule.anchored = true;
    pattern.erase(0, 1);
  }
  rule.pattern = std::move(pattern);
  if (!rule.pattern.empty()) rules_.push_back(std::move(rule));
}

void GitIgnore::add_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  const auto base = absolute_normalized(path).parent_path();
  while (std::getline(in, line)) add_line(line, base);
}

bool GitIgnore::ignored(const std::filesystem::path& path, bool is_directory) const {
  bool ignored = false;
  for (const auto& rule : rules_) {
    if (match_rule(rule, path, is_directory)) ignored = !rule.negated;
  }
  return ignored;
}

GitIgnore load_gitignore_stack(const std::filesystem::path& root) {
  GitIgnore stack;
  const auto absolute_root = absolute_normalized(root);
  std::vector<std::filesystem::path> directories;
  bool found_repository = false;
  for (auto dir = absolute_root;; dir = dir.parent_path()) {
    directories.push_back(dir);
    std::error_code ec;
    if (std::filesystem::exists(dir / ".git", ec) && !ec) {
      found_repository = true;
      break;
    }
    if (!dir.has_parent_path() || dir == dir.parent_path()) break;
  }
  if (!found_repository) directories = {absolute_root};

  std::reverse(directories.begin(), directories.end());
  for (const auto& dir : directories) {
    std::error_code ec;
    const auto ignore_file = dir / ".gitignore";
    if (std::filesystem::is_regular_file(ignore_file, ec) && !ec) stack.add_file(ignore_file);
  }
  return stack;
}

}  // namespace kiko
