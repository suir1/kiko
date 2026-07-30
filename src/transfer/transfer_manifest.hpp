#pragma once

#include "transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kiko::detail {

inline constexpr std::size_t kMaxTransferManifestBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxTransferManifestEntries = 65536;
inline constexpr std::size_t kMaxTransferPathBytes = 4096;
inline constexpr std::size_t kMaxTransferMetadataBytes = 4096;

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
