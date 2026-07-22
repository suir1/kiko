#include "transfer_manifest.hpp"

#include "core/common.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace kiko::detail {
namespace {

std::string manifest_string_field(const nlohmann::json& object, const char* key, const std::string& fallback = {}) {
  if (!object.contains(key)) return fallback;
  const auto& value = object.at(key);
  if (!value.is_string()) throw KikoError(std::string("manifest field must be a string: ") + key);
  return value.get<std::string>();
}

std::uint64_t manifest_u64_field(const nlohmann::json& object, const char* key, std::uint64_t fallback = 0) {
  if (!object.contains(key)) return fallback;
  const auto& value = object.at(key);
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) throw KikoError(std::string("manifest field must be unsigned: ") + key);
    return static_cast<std::uint64_t>(signed_value);
  }
  if (value.is_string()) {
    auto parsed = parse_u64_strict(value.get<std::string>());
    if (parsed) return *parsed;
  }
  throw KikoError(std::string("manifest field must be an unsigned integer: ") + key);
}

std::uint32_t manifest_u32_field(const nlohmann::json& object, const char* key, std::uint32_t fallback = 0) {
  const auto value = manifest_u64_field(object, key, fallback);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw KikoError(std::string("manifest field is too large: ") + key);
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

void add_manifest_size(std::uint64_t& total, std::uint64_t size, const std::string& relative) {
  if (size > std::numeric_limits<std::uint64_t>::max() - total) {
    throw KikoError("manifest total size overflow near " + relative);
  }
  total += size;
}

std::string encode_transfer_manifest(const std::vector<FileEntry>& files) {
  nlohmann::json root = nlohmann::json::object();
  root["version"] = 1;
  root["entries"] = nlohmann::json::array();

  std::uint64_t total_size = 0;
  for (const auto& entry : files) {
    nlohmann::json item = nlohmann::json::object();
    item["path"] = entry.relative;
    item["kind"] = transfer_entry_kind_name(transfer_entry_kind(entry));
    item["size"] = entry.size;
    if (!entry.imohash.empty()) item["imohash"] = entry.imohash;
    if (!entry.link_target.empty()) item["target"] = entry.link_target;
    if (entry.mtime_ms > 0) item["mtime_ms"] = entry.mtime_ms;
    if (entry.mode > 0) item["mode"] = entry.mode;
    root["entries"].push_back(std::move(item));
    if (transfer_entry_kind(entry) == TransferEntryKind::File) {
      add_manifest_size(total_size, entry.size, entry.relative);
    }
  }

  root["count"] = files.size();
  root["total_size"] = total_size;
  return root.dump();
}

TransferManifest decode_transfer_manifest(std::string_view text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(std::string(text));
  } catch (const nlohmann::json::exception& error) {
    throw KikoError(std::string("invalid transfer manifest json: ") + error.what());
  }
  if (!root.is_object()) throw KikoError("transfer manifest must be a json object");
  const auto version = manifest_u64_field(root, "version", 1);
  if (version != 1) throw KikoError("unsupported transfer manifest version: " + std::to_string(version));
  if (!root.contains("entries") || !root["entries"].is_array()) {
    throw KikoError("transfer manifest missing entries");
  }

  TransferManifest manifest;
  std::uint64_t computed_total = 0;
  for (const auto& item : root["entries"]) {
    if (!item.is_object()) throw KikoError("transfer manifest entry must be a json object");
    TransferManifestEntry entry;
    entry.path = manifest_string_field(item, "path");
    entry.kind = manifest_string_field(item, "kind", "file");
    entry.size = manifest_u64_field(item, "size", 0);
    entry.target = manifest_string_field(item, "target");
    entry.imohash = manifest_string_field(item, "imohash");
    entry.mtime_ms = manifest_u64_field(item, "mtime_ms", 0);
    entry.mode = manifest_u32_field(item, "mode", 0);
    if (entry.kind == "file") add_manifest_size(computed_total, entry.size, entry.path);
    manifest.entries.push_back(std::move(entry));
  }

  const auto declared_count = manifest_u64_field(root, "count", manifest.entries.size());
  if (declared_count != manifest.entries.size()) {
    throw KikoError("transfer manifest count mismatch");
  }
  const auto declared_total = manifest_u64_field(root, "total_size", computed_total);
  if (declared_total != computed_total) {
    throw KikoError("transfer manifest total_size mismatch");
  }
  manifest.total_size = computed_total;
  return manifest;
}

}  // namespace kiko::detail
