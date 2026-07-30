#pragma once

#include "core/progress.hpp"
#include "core/protocol.hpp"
#include "transfer_manifest.hpp"
#include "transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace kiko::detail {

enum class ReceivePlanAction {
  Write,
  Skip,
  Rename,
};

struct ReceivePlanEntry {
  TransferManifestEntry manifest;
  std::filesystem::path target_path;
  ReceivePlanAction action = ReceivePlanAction::Write;
};

struct ReceivePlan {
  std::map<std::string, ReceivePlanEntry> entries;
  std::set<std::string> received_paths;
};

[[nodiscard]] ReceivePlan preflight_transfer_manifest(const TransferManifest& manifest,
                                                      const std::filesystem::path& output_dir,
                                                      ConflictPolicy conflict_policy, ProgressReporter& reporter);
[[nodiscard]] const ReceivePlanEntry* find_receive_plan_entry(const ReceivePlan* plan, const std::string& relative);
void record_receive_plan_header(ReceivePlan* plan, const std::string& relative);
void validate_receive_plan_complete(const ReceivePlan* plan);
void validate_receive_plan_header(const ReceivePlanEntry& planned, const Message& header,
                                  const std::string& relative, std::uint64_t declared_size);

}  // namespace kiko::detail
