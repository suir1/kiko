#include "transfer_receive_paths.hpp"

#include "platform/atomic_file.hpp"

#include <fstream>
#include <span>

namespace kiko::detail {

bool is_safe_relative_symlink_target(const std::filesystem::path& target) {
  if (target.empty() || target.is_absolute()) return false;
  for (const auto& part : target) {
    if (part == "..") return false;
  }
  return true;
}

namespace {

struct TemporaryPathCleanup {
  std::filesystem::path path;

  ~TemporaryPathCleanup() {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

bool path_has_prefix(const std::filesystem::path& base, const std::filesystem::path& path) {
  auto base_it = base.begin();
  auto path_it = path.begin();
  for (; base_it != base.end(); ++base_it, ++path_it) {
    if (path_it == path.end() || *base_it != *path_it) return false;
  }
  return true;
}

std::filesystem::path absolute_normalized(const std::filesystem::path& path, const std::string& relative) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec) throw KikoError("failed to resolve receive path for " + relative + ": " + ec.message());
  return absolute.lexically_normal();
}

std::string sha256_file_hex(const std::filesystem::path& path, Bytes& buffer) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw KikoError("failed to open received file for verification: " + path.string());

  Sha256Hasher hasher;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    auto got = input.gcount();
    if (got <= 0) break;
    hasher.update(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(got)));
  }
  return hex_encode(hasher.finish());
}

}  // namespace

bool is_dir_header(const std::string& path, std::uint64_t size) {
  return size == 0 && !path.empty() && path.back() == '/';
}

bool is_symlink_header(const Message& header) {
  return header.type == "symlink" || header.get("kind") == "symlink";
}

bool path_exists_no_follow(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  return !ec && std::filesystem::exists(status);
}

std::filesystem::path unique_conflict_path(
    const std::filesystem::path& path,
    const std::function<bool(const std::filesystem::path&)>& reserved) {
  const auto parent = path.parent_path();
  auto stem = path.stem().string();
  const auto extension = path.extension().string();
  if (stem.empty()) stem = path.filename().string();
  if (stem.empty()) stem = "received";

  for (int i = 1; i < 10000; ++i) {
    auto candidate = parent / (stem + " (" + std::to_string(i) + ")" + extension);
    auto candidate_part = candidate;
    candidate_part += ".kikopart";
    if ((!reserved || !reserved(candidate)) && !path_exists_no_follow(candidate) &&
        !path_exists_no_follow(candidate_part)) {
      return candidate;
    }
  }
  throw KikoError("could not choose a non-conflicting filename for " + path.string());
}

void report_renamed_conflict(const std::string& relative, const std::filesystem::path& renamed,
                             ProgressReporter& reporter) {
  reporter.status("renamed conflict " + relative + " -> " + renamed.filename().generic_string());
}

void verify_received_digest(const std::filesystem::path& part_path, const std::string& relative,
                            std::uint64_t received_size, std::uint64_t declared_size, const std::string& expected_sha256,
                            const std::string& actual_sha256) {
  if (received_size != declared_size) {
    std::error_code ec;
    std::filesystem::remove(part_path, ec);
    throw KikoError("received " + std::to_string(received_size) + " bytes for " + relative + ", expected " +
                    std::to_string(declared_size));
  }
  std::error_code size_ec;
  const auto stored_size = std::filesystem::file_size(part_path, size_ec);
  if (size_ec || stored_size != static_cast<std::uintmax_t>(declared_size)) {
    std::error_code remove_ec;
    std::filesystem::remove(part_path, remove_ec);
    const auto actual_size = size_ec ? std::string("unknown") : std::to_string(stored_size);
    throw KikoError("received file size mismatch for " + relative + " (stored " + actual_size + ", expected " +
                    std::to_string(declared_size) + ")");
  }
  if (actual_sha256 != expected_sha256) {
    std::error_code ec;
    std::filesystem::remove(part_path, ec);
    throw KikoError("integrity check failed for " + relative + " (expected " + expected_sha256 + ", got " +
                    actual_sha256 + ")");
  }
}

void verify_part_file_digest(const std::filesystem::path& part_path, const std::string& relative,
                             std::uint64_t declared_size, const std::string& expected_sha256, Bytes& buffer) {
  std::error_code size_ec;
  auto received_size = std::filesystem::file_size(part_path, size_ec);
  if (size_ec) {
    std::error_code remove_ec;
    std::filesystem::remove(part_path, remove_ec);
    throw KikoError("received 0 bytes for " + relative + ", expected " + std::to_string(declared_size));
  }

  const auto actual_sha256 = sha256_file_hex(part_path, buffer);
  verify_received_digest(part_path, relative, static_cast<std::uint64_t>(received_size), declared_size, expected_sha256,
                         actual_sha256);
}

void finalize_part_file(const std::filesystem::path& part_path, const std::filesystem::path& current_path) {
  if (auto error = atomic_replace_file(part_path, current_path)) {
    throw KikoError("failed to finalize file " + current_path.string() + ": " + *error);
  }
}

void validate_safe_symlink_target(const std::string& relative, const std::string& target) {
  if (relative.empty() || relative.back() == '/') {
    throw KikoError("refusing invalid symlink path: " + relative);
  }
  if (target.find('\0') != std::string::npos) {
    throw KikoError("refusing null byte in symlink target for " + relative);
  }
  const std::filesystem::path link_target(target);
  if (!is_safe_relative_symlink_target(link_target)) {
    throw KikoError("refusing unsafe symlink target for " + relative + ": " + target);
  }
}

void create_safe_symlink(const std::filesystem::path& current_path, const std::string& relative,
                         const std::string& target) {
  validate_safe_symlink_target(relative, target);
  const std::filesystem::path link_target(target);
  std::error_code ec;
  if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());
  auto temporary = current_path;
  temporary += ".linktmp-" + random_code(12);
  TemporaryPathCleanup cleanup{temporary};
  std::filesystem::create_symlink(link_target, temporary, ec);
  if (ec) throw KikoError("failed to create temporary symlink for " + current_path.string() + ": " + ec.message());
  if (auto error = atomic_replace_file(temporary, current_path)) {
    throw KikoError("failed to finalize symlink " + current_path.string() + ": " + *error);
  }
  cleanup.path.clear();
}

std::filesystem::path safe_join(const std::filesystem::path& base, const std::string& relative) {
  if (relative.find('\0') != std::string::npos) {
    throw KikoError("refusing null byte in transfer path");
  }
  std::filesystem::path rel(relative);
  if (rel.is_absolute()) throw KikoError("refusing absolute path in transfer: " + relative);
  for (const auto& part : rel) {
    if (part == "..") throw KikoError("refusing path traversal in transfer: " + relative);
  }
  return base / rel;
}

void validate_receive_target_parent(const std::filesystem::path& output_dir,
                                    const std::filesystem::path& target_path,
                                    const std::string& relative) {
  const auto root = absolute_normalized(output_dir, relative);
  const auto target = absolute_normalized(target_path, relative);
  if (!path_has_prefix(root, target)) {
    throw KikoError("refusing receive target outside output directory: " + relative);
  }

  const auto parent = target.parent_path();
  std::error_code ec;
  const auto resolved_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) throw KikoError("failed to resolve receive root for " + relative + ": " + ec.message());
  ec.clear();
  const auto resolved_parent = std::filesystem::weakly_canonical(parent, ec);
  if (ec || !path_has_prefix(resolved_root, resolved_parent)) {
    throw KikoError("refusing receive path through a symbolic link outside output directory: " + relative);
  }

  auto relative_parent = parent.lexically_relative(root);
  auto current = root;
  for (const auto& part : relative_parent) {
    if (part == ".") continue;
    if (part == "..") throw KikoError("refusing receive parent traversal: " + relative);
    current /= part;
    ec.clear();
    const auto status = std::filesystem::symlink_status(current, ec);
    if (ec) {
      if (ec == std::errc::no_such_file_or_directory) continue;
      throw KikoError("failed to inspect receive parent for " + relative + ": " + ec.message());
    }
    if (!std::filesystem::exists(status)) continue;
    if (std::filesystem::is_symlink(status)) {
      throw KikoError("refusing symbolic link in receive parent path: " + relative);
    }
    if (!std::filesystem::is_directory(status)) {
      throw KikoError("receive parent is not a directory for " + relative + ": " + current.string());
    }
  }
}

void validate_receive_part_path(const std::filesystem::path& part_path, const std::string& relative) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(part_path, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) return;
    throw KikoError("failed to inspect partial file for " + relative + ": " + ec.message());
  }
  if (!std::filesystem::exists(status)) return;
  if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
    throw KikoError("refusing unsafe partial file for " + relative + ": " + part_path.string());
  }
}

}  // namespace kiko::detail
