#include "receive_plan.hpp"

#include "transfer_receive_paths.hpp"
#include "transfer_resume.hpp"

#include <limits>
#include <set>

namespace kiko::detail {
namespace {

void ensure_manifest_total(std::uint64_t& total, std::uint64_t size, const std::string& relative) {
  if (size > std::numeric_limits<std::uint64_t>::max() - total) {
    throw KikoError("manifest total size overflow near " + relative);
  }
  total += size;
}

std::string target_key(const std::filesystem::path& path) {
  auto key = path.lexically_normal().generic_string();
  while (key.size() > 1 && key.back() == '/') key.pop_back();
  return key;
}

bool key_is_parent_of(const std::string& parent, const std::string& child) {
  return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/';
}

void ensure_no_plan_collision(const std::string& key, bool directory, const std::set<std::string>& reserved_targets,
                              const std::set<std::string>& reserved_files, const std::string& relative) {
  if (reserved_targets.contains(key)) throw KikoError("receive plan target collision for " + relative);

  for (const auto& file_key : reserved_files) {
    if (key_is_parent_of(file_key, key)) {
      throw KikoError("receive plan parent is already a file for " + relative);
    }
    if (!directory && key_is_parent_of(key, file_key)) {
      throw KikoError("receive plan target would replace a planned directory for " + relative);
    }
  }
}

void ensure_existing_parents_are_directories(const std::filesystem::path& path, const std::string& relative) {
  auto parent = path.parent_path();
  while (!parent.empty()) {
    std::error_code ec;
    const auto status = std::filesystem::status(parent, ec);
    if (!ec && std::filesystem::exists(status) && !std::filesystem::is_directory(status)) {
      throw KikoError("receive plan parent is not a directory for " + relative + ": " + parent.string());
    }
    const auto next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }
}

std::filesystem::path unique_conflict_path_reserved(const std::filesystem::path& path,
                                                    const std::set<std::string>& reserved_targets) {
  const auto parent = path.parent_path();
  auto stem = path.stem().string();
  const auto extension = path.extension().string();
  if (stem.empty()) stem = path.filename().string();
  if (stem.empty()) stem = "received";

  for (int i = 1; i < 10000; ++i) {
    auto candidate = parent / (stem + " (" + std::to_string(i) + ")" + extension);
    auto candidate_part = candidate;
    candidate_part += ".kikopart";
    if (!reserved_targets.contains(target_key(candidate)) && !path_exists_no_follow(candidate) &&
        !path_exists_no_follow(candidate_part)) {
      return candidate;
    }
  }
  throw KikoError("could not choose a non-conflicting filename for " + path.string());
}

}  // namespace

ReceivePlan preflight_transfer_manifest(const TransferManifest& manifest, const std::filesystem::path& output_dir,
                                        ConflictPolicy conflict_policy, ProgressReporter& reporter) {
  std::set<std::string> seen_paths;
  std::set<std::string> reserved_targets;
  std::set<std::string> reserved_files;
  std::uint64_t computed_total = 0;
  ReceivePlan plan;
  ReceivePlanSummary summary;
  summary.item_count = manifest.entries.size();
  summary.total_bytes = manifest.total_size;

  for (const auto& entry : manifest.entries) {
    if (entry.path.empty()) throw KikoError("transfer manifest entry missing path");
    if (!seen_paths.insert(entry.path).second) {
      throw KikoError("transfer manifest contains duplicate path: " + entry.path);
    }

    auto current_path = safe_join(output_dir, entry.path);
    auto target_path = current_path;
    auto action = ReceivePlanAction::Write;
    const bool existing_target = path_exists_no_follow(current_path);

    if (entry.kind == "symlink") {
      if (entry.size != 0) throw KikoError("transfer manifest symlink must have size 0: " + entry.path);
      validate_safe_symlink_target(entry.path, entry.target);
    } else if (entry.kind == "dir") {
      if (entry.path.back() != '/') throw KikoError("transfer manifest directory path must end with /: " + entry.path);
      if (entry.size != 0) throw KikoError("transfer manifest directory must have size 0: " + entry.path);
    } else if (entry.kind == "file") {
      if (entry.path.back() == '/') throw KikoError("transfer manifest file path must not end with /: " + entry.path);
      ensure_manifest_total(computed_total, entry.size, entry.path);
    } else {
      throw KikoError("transfer manifest contains unknown entry kind: " + entry.kind);
    }

    if (entry.kind == "dir") {
      std::error_code ec;
      const auto status = std::filesystem::symlink_status(current_path, ec);
      if (!ec && std::filesystem::exists(status) && !std::filesystem::is_directory(status)) {
        throw KikoError("receive plan directory target is not a directory: " + entry.path);
      }
    } else if (existing_target && conflict_policy == ConflictPolicy::Skip) {
      action = ReceivePlanAction::Skip;
    } else if (existing_target && conflict_policy == ConflictPolicy::Rename) {
      action = ReceivePlanAction::Rename;
      target_path = unique_conflict_path_reserved(current_path, reserved_targets);
    } else if (existing_target) {
      std::error_code ec;
      const auto status = std::filesystem::symlink_status(current_path, ec);
      if (!ec && std::filesystem::is_directory(status)) {
        throw KikoError("receive plan file target is a directory: " + entry.path);
      }
      ++summary.overwrite_count;
    }

    ensure_existing_parents_are_directories(target_path, entry.path);

    std::uint64_t planned_resume = 0;
    if (action == ReceivePlanAction::Write && entry.kind == "file") {
      planned_resume = resumable_part_size(part_path_for(target_path), entry.size);
      if (planned_resume > 0) {
        ++summary.resume_count;
        summary.resume_bytes += planned_resume;
      }
    }

    if (action == ReceivePlanAction::Skip) {
      ++summary.skip_count;
      summary.skip_bytes += entry.kind == "file" ? entry.size : 0;
    } else {
      const auto key = target_key(target_path);
      const bool directory = entry.kind == "dir";
      ensure_no_plan_collision(key, directory, reserved_targets, reserved_files, entry.path);
      reserved_targets.insert(key);
      if (!directory) reserved_files.insert(key);
      if (action == ReceivePlanAction::Rename) ++summary.rename_count;
    }

    plan.entries.emplace(entry.path, ReceivePlanEntry{entry, target_path, action, planned_resume});
  }

  if (computed_total != manifest.total_size) throw KikoError("transfer manifest total size changed during preflight");
  reporter.status("manifest: " + std::to_string(manifest.entries.size()) + " item(s), " +
                  std::to_string(manifest.total_size) + " bytes");
  reporter.receive_plan(summary);
  return plan;
}

const ReceivePlanEntry* find_receive_plan_entry(const ReceivePlan* plan, const std::string& relative) {
  if (plan == nullptr) return nullptr;
  auto it = plan->entries.find(relative);
  return it == plan->entries.end() ? nullptr : &it->second;
}

void validate_receive_plan_header(const ReceivePlanEntry& planned, const Message& header, const std::string& relative,
                                  std::uint64_t declared_size) {
  std::string header_kind = "file";
  if (is_symlink_header(header)) {
    header_kind = "symlink";
  } else if (is_dir_header(relative, declared_size)) {
    header_kind = "dir";
  }

  if (planned.manifest.kind != header_kind) {
    throw KikoError("file header kind does not match manifest for " + relative);
  }
  if (planned.manifest.size != declared_size) {
    throw KikoError("file header size does not match manifest for " + relative);
  }
  if (header_kind == "symlink" && planned.manifest.target != header.get("target")) {
    throw KikoError("file header symlink target does not match manifest for " + relative);
  }
}

}  // namespace kiko::detail
