#pragma once

#include "progress.hpp"
#include "protocol.hpp"
#include "transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kiko::detail {

struct TransferManifestEntry {
  std::string path;
  std::uint64_t size = 0;
  std::string kind;
  std::string target;
  std::string imohash;
  std::uint64_t mtime_ms = 0;
  std::uint32_t mode = 0;
};

struct TransferManifest {
  std::vector<TransferManifestEntry> entries;
  std::uint64_t total_size = 0;
};

enum class ReceivePlanAction {
  Write,
  Skip,
  Rename,
};

struct ReceivePlanEntry {
  TransferManifestEntry manifest;
  std::filesystem::path target_path;
  ReceivePlanAction action = ReceivePlanAction::Write;
  std::uint64_t resume_offset = 0;
};

struct ReceivePlan {
  std::map<std::string, ReceivePlanEntry> entries;
  std::uint64_t total_size = 0;
  std::uint64_t resume_bytes = 0;
  std::uint64_t skip_bytes = 0;
  std::size_t write_count = 0;
  std::size_t skip_count = 0;
  std::size_t rename_count = 0;
  std::size_t overwrite_count = 0;
  std::size_t resume_count = 0;
};

[[nodiscard]] ReceivePlan preflight_transfer_manifest(const TransferManifest& manifest,
                                                      const std::filesystem::path& output_dir,
                                                      ConflictPolicy conflict_policy, ProgressReporter& reporter);
[[nodiscard]] const ReceivePlanEntry* find_receive_plan_entry(const ReceivePlan* plan, const std::string& relative);
void validate_receive_plan_header(const ReceivePlanEntry& planned, const Message& header,
                                  const std::string& relative, std::uint64_t declared_size);

}  // namespace kiko::detail
