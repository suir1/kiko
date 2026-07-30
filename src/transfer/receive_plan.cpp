#include "receive_plan.hpp"

#include "transfer_receive_paths.hpp"
#include "transfer_resume.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <set>

namespace kiko::detail {
namespace {

constexpr std::uint64_t kReceiveFreeSpaceReserveBytes = 64ull * 1024ull * 1024ull;

std::string target_key(const std::filesystem::path& path, bool fold_ascii_case = false) {
  auto key = path.lexically_normal().generic_string();
  while (key.size() > 1 && key.back() == '/') key.pop_back();
  if (fold_ascii_case) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
  }
  return key;
}

bool key_is_parent_of(const std::string& parent, const std::string& child) {
  return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/';
}

std::optional<std::string> plan_collision_error(const std::filesystem::path& target, bool directory,
                                                const std::set<std::string>& reserved_targets,
                                                const std::set<std::string>& reserved_files,
                                                bool fold_ascii_case) {
  const auto key = target_key(target, fold_ascii_case);
  if (reserved_targets.contains(key)) return "target collision";

  auto parent = target.parent_path();
  while (!parent.empty()) {
    if (reserved_files.contains(target_key(parent, fold_ascii_case))) return "parent is already a file";
    const auto next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }

  if (!directory) {
    const auto child = reserved_targets.lower_bound(key + "/");
    if (child != reserved_targets.end() && key_is_parent_of(key, *child)) {
      return "target would replace a planned directory";
    }
  }
  return std::nullopt;
}

bool receive_filesystem_is_case_insensitive(const std::filesystem::path& output_dir) {
  std::error_code ec;
  auto probe_parent = std::filesystem::absolute(output_dir, ec);
  if (ec) throw KikoError("failed to resolve receive directory for case-sensitivity check: " + ec.message());

  while (true) {
    ec.clear();
    if (std::filesystem::is_directory(probe_parent, ec)) break;
    if (probe_parent == probe_parent.root_path()) {
      throw KikoError("could not find receive filesystem for case-sensitivity check");
    }
    probe_parent = probe_parent.parent_path();
  }

  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto stem = ".kiko-case-probe-" + random_code(8);
    const auto lower = probe_parent / (stem + "a");
    const auto upper = probe_parent / (stem + "A");
    ec.clear();
    if (!std::filesystem::create_directory(lower, ec)) {
      if (!ec || ec == std::errc::file_exists) continue;
      throw KikoError("failed to probe receive filesystem case sensitivity: " + ec.message());
    }

    std::error_code status_error;
    const bool insensitive = std::filesystem::exists(upper, status_error);
    std::error_code remove_error;
    std::filesystem::remove(lower, remove_error);
    if (status_error) {
      throw KikoError("failed to inspect receive filesystem case sensitivity: " + status_error.message());
    }
    if (remove_error) {
      throw KikoError("failed to clean receive filesystem case-sensitivity probe: " + remove_error.message());
    }
    return insensitive;
  }
  throw KikoError("could not allocate receive filesystem case-sensitivity probe");
}

void add_saturating(std::uint64_t& total, std::uintmax_t value) {
  const auto max = std::numeric_limits<std::uint64_t>::max();
  if (value >= static_cast<std::uintmax_t>(max - total)) {
    total = max;
  } else {
    total += static_cast<std::uint64_t>(value);
  }
}

std::uintmax_t reclaimable_target_size(const std::filesystem::path& target) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(target, ec);
  if (ec || !std::filesystem::is_regular_file(status)) return 0;
  const auto size = std::filesystem::file_size(target, ec);
  return ec ? 0 : size;
}

void ensure_receive_space(const std::filesystem::path& output_dir, std::uint64_t peak_required_bytes) {
  if (peak_required_bytes == 0) return;

  std::error_code ec;
  auto probe = std::filesystem::absolute(output_dir, ec);
  if (ec) throw KikoError("failed to inspect free space for receive directory: " + ec.message());

  std::filesystem::space_info space;
  while (true) {
    ec.clear();
    space = std::filesystem::space(probe, ec);
    if (!ec) break;
    if (ec != std::errc::no_such_file_or_directory || probe == probe.root_path()) {
      throw KikoError("failed to inspect free space for receive directory: " + ec.message());
    }
    probe = probe.parent_path();
  }

  const auto reserve = static_cast<std::uintmax_t>(kReceiveFreeSpaceReserveBytes);
  const auto usable = space.available > reserve ? space.available - reserve : 0;
  if (static_cast<std::uintmax_t>(peak_required_bytes) > usable) {
    throw KikoError("not enough free space to receive files (need " + std::to_string(peak_required_bytes) +
                    " bytes plus reserve, available " + std::to_string(space.available) + ")");
  }
}

}  // namespace

ReceivePlan preflight_transfer_manifest(const TransferManifest& manifest, const std::filesystem::path& output_dir,
                                        ConflictPolicy conflict_policy, ProgressReporter& reporter) {
  std::set<std::string> seen_paths;
  std::set<std::string> reserved_targets;
  std::set<std::string> reserved_files;
  std::set<std::string> folded_targets;
  std::set<std::string> folded_files;
  std::optional<bool> case_insensitive_filesystem;
  std::uint64_t computed_total = 0;
  std::uint64_t cumulative_write_bytes = 0;
  std::uint64_t cumulative_reclaimed_bytes = 0;
  std::uint64_t peak_required_bytes = 0;
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
      add_manifest_size(computed_total, entry.size, entry.path);
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
      target_path = unique_conflict_path(current_path, [&](const std::filesystem::path& candidate) {
        return reserved_targets.contains(target_key(candidate));
      });
    } else if (existing_target) {
      std::error_code ec;
      const auto status = std::filesystem::symlink_status(current_path, ec);
      if (!ec && std::filesystem::is_directory(status)) {
        throw KikoError("receive plan file target is a directory: " + entry.path);
      }
      ++summary.overwrite_count;
    }

    validate_receive_target_parent(output_dir, target_path, entry.path);

    std::uint64_t planned_resume = 0;
    if (action == ReceivePlanAction::Write && entry.kind == "file") {
      const auto part_path = part_path_for(target_path);
      validate_receive_part_path(part_path, entry.path);
      planned_resume = resumable_part_size(part_path, entry.size);
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
      if (const auto collision = plan_collision_error(target_path, directory, reserved_targets, reserved_files, false)) {
        throw KikoError("receive plan " + *collision + " for " + entry.path);
      }
      if (case_insensitive_filesystem.value_or(true)) {
        if (const auto collision = plan_collision_error(target_path, directory, folded_targets, folded_files, true)) {
          if (!case_insensitive_filesystem) {
            case_insensitive_filesystem = receive_filesystem_is_case_insensitive(output_dir);
          }
          if (*case_insensitive_filesystem) {
            throw KikoError("receive plan case-insensitive " + *collision + " for " + entry.path);
          }
        }
      }
      reserved_targets.insert(key);
      folded_targets.insert(target_key(target_path, true));
      if (!directory) reserved_files.insert(key);
      if (!directory) folded_files.insert(target_key(target_path, true));
      if (action == ReceivePlanAction::Rename) ++summary.rename_count;
    }

    if (entry.kind == "file" && action != ReceivePlanAction::Skip) {
      const auto remaining = entry.size - planned_resume;
      add_manifest_size(cumulative_write_bytes, remaining, entry.path);
      const auto live_required = cumulative_write_bytes > cumulative_reclaimed_bytes
                                     ? cumulative_write_bytes - cumulative_reclaimed_bytes
                                     : 0;
      peak_required_bytes = std::max(peak_required_bytes, live_required);
      if (existing_target && action == ReceivePlanAction::Write) {
        add_saturating(cumulative_reclaimed_bytes, reclaimable_target_size(target_path));
      }
    }

    plan.entries.emplace(entry.path, ReceivePlanEntry{entry, target_path, action});
  }

  if (computed_total != manifest.total_size) throw KikoError("transfer manifest total size changed during preflight");
  ensure_receive_space(output_dir, peak_required_bytes);
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

void record_receive_plan_header(ReceivePlan* plan, const std::string& relative) {
  if (plan == nullptr) return;
  if (!plan->entries.contains(relative)) {
    throw KikoError("file header was not listed in manifest: " + relative);
  }
  if (!plan->received_paths.insert(relative).second) {
    throw KikoError("duplicate file header for manifest entry: " + relative);
  }
}

void validate_receive_plan_complete(const ReceivePlan* plan) {
  if (plan == nullptr || plan->received_paths.size() == plan->entries.size()) return;
  for (const auto& [relative, _] : plan->entries) {
    if (!plan->received_paths.contains(relative)) {
      throw KikoError("transfer ended before manifest entry: " + relative);
    }
  }
  throw KikoError("transfer manifest completion mismatch");
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
