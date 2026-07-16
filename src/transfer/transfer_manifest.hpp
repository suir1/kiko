#pragma once

#include "transfer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
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

void add_manifest_size(std::uint64_t& total, std::uint64_t size, const std::string& relative);
[[nodiscard]] std::string encode_transfer_manifest(const std::vector<FileEntry>& files);
[[nodiscard]] TransferManifest decode_transfer_manifest(std::string_view text);

}  // namespace kiko::detail
