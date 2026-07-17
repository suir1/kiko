#include "transfer_receive_paths.hpp"

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
  std::error_code ec;
  std::filesystem::rename(part_path, current_path, ec);
  if (ec) {
    std::filesystem::remove(current_path, ec);
    std::filesystem::rename(part_path, current_path, ec);
    if (ec) throw KikoError("failed to finalize file: " + current_path.string());
  }
}

void validate_safe_symlink_target(const std::string& relative, const std::string& target) {
  if (relative.empty() || relative.back() == '/') {
    throw KikoError("refusing invalid symlink path: " + relative);
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
  std::filesystem::remove(current_path, ec);
  ec.clear();
  std::filesystem::create_symlink(link_target, current_path, ec);
  if (ec) throw KikoError("failed to create symlink: " + current_path.string());
}

std::filesystem::path safe_join(const std::filesystem::path& base, const std::string& relative) {
  std::filesystem::path rel(relative);
  if (rel.is_absolute()) throw KikoError("refusing absolute path in transfer: " + relative);
  for (const auto& part : rel) {
    if (part == "..") throw KikoError("refusing path traversal in transfer: " + relative);
  }
  return base / rel;
}

}  // namespace kiko::detail
