#include "transfer_file_session.hpp"

#include "file_metadata.hpp"
#include "transfer_heuristics.hpp"
#include "transfer_receive_paths.hpp"
#include "transfer_resume.hpp"
#include "transfer_stream.hpp"

#include <utility>

namespace kiko::detail {
namespace {

void append_mtime_field(Message& header, const FileEntry& entry) {
  if (entry.mtime_ms > 0) header.fields["mtime_ms"] = std::to_string(entry.mtime_ms);
}

void append_mode_field(Message& header, const FileEntry& entry) {
  if (entry.mode > 0) header.fields["mode"] = std::to_string(entry.mode);
}

}  // namespace

bool is_dir_entry(const FileEntry& entry) {
  return entry.size == 0 && !entry.relative.empty() && entry.relative.back() == '/';
}

bool is_symlink_entry(const FileEntry& entry) {
  return entry.symlink;
}

namespace {

bool should_compress_entry(const FileEntry& entry) {
  return !is_dir_entry(entry) && !is_symlink_entry(entry) && should_compress_path(entry.absolute);
}

Message make_file_header(const FileEntry& entry) {
  if (is_symlink_entry(entry)) {
    return Message{"file",
                   {{"path", entry.relative},
                    {"size", "0"},
                    {"compress", "none"},
                    {"kind", "symlink"},
                    {"target", entry.link_target}}};
  }
  if (is_dir_entry(entry)) {
    Message header{"file", {{"path", entry.relative}, {"size", "0"}, {"compress", "none"}}};
    append_mtime_field(header, entry);
    append_mode_field(header, entry);
    return header;
  }

  Message header{"file",
                 {{"path", entry.relative},
                  {"size", std::to_string(entry.size)},
                  {"imohash", entry.imohash},
                  {"compress", should_compress_entry(entry) ? "zstd" : "none"}}};
  append_mtime_field(header, entry);
  append_mode_field(header, entry);
  return header;
}

}  // namespace

SendFileSession::SendFileSession(const FileEntry& entry, TcpSocket& control, StreamCipher& cipher,
                                 ProgressReporter& reporter, TransferTiming& timing, Bytes& buffer)
    : entry_(entry), control_(control), cipher_(cipher), reporter_(reporter), timing_(timing) {
  const bool marker = is_symlink_entry(entry_) || is_dir_entry(entry_);
  if (!marker) {
    input_.open(entry_.absolute, std::ios::binary);
    if (!input_) throw KikoError("failed to open input file: " + entry_.absolute.string());
  }

  auto header = make_file_header(entry_);
  send_tagged_text_timed(control_, cipher_, StreamTag::FileHeader, encode_message(header), timing_);
  reporter_.file_start(entry_.relative, marker ? 0 : entry_.size);

  const auto resume = recv_resume_request(control_, cipher_, entry_);
  if (marker) {
    send_resume_ack(control_, cipher_, resume.offset);
    action_ = SendFileAction::MarkerComplete;
    (void)finish(kEmptySha256, 0);
    return;
  }

  if (resume.complete_skip && resume.offset == entry_.size) {
    send_resume_ack(control_, cipher_, entry_.size);
    reporter_.status("skipped already-complete " + entry_.relative);
    reporter_.file_resume(entry_.relative, entry_.size, entry_.size);
    reporter_.file_advance(entry_.size);
    total_ = entry_.size;
    action_ = SendFileAction::SkipPayload;
    return;
  }

  hasher_.emplace();
  std::string source_prefix_sha256;
  total_ = hash_stream_prefix(input_, buffer, *hasher_, resume.offset, entry_.relative,
                              resume.offset > 0 ? &source_prefix_sha256 : nullptr);
  auto offset = resume.offset;
  if (offset > 0 && !resume.prefix_sha256.empty() && resume.prefix_sha256 != source_prefix_sha256) {
    reporter_.status("resume prefix mismatch, restarting " + entry_.relative);
    input_.clear();
    input_.seekg(0);
    hasher_.emplace();
    total_ = 0;
    offset = 0;
  }
  send_resume_ack(control_, cipher_, offset);
  if (offset > 0) {
    reporter_.file_resume(entry_.relative, offset, entry_.size);
    reporter_.file_advance(offset);
  }

  use_zstd_ = should_compress_entry(entry_);
  action_ = SendFileAction::SendPayload;
}

std::optional<SendFileChunk> SendFileSession::read_next(Bytes& buffer) {
  const auto read_start = TransferClock::now();
  input_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
  add_transfer_elapsed(timing_.disk_read_ms, read_start);
  const auto got = input_.gcount();
  if (got <= 0) return std::nullopt;

  const auto offset = total_;
  std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(got));
  const auto hash_start = TransferClock::now();
  hasher_->update(chunk);
  add_transfer_elapsed(timing_.hash_ms, hash_start);
  total_ += static_cast<std::uint64_t>(got);
  timing_.payload_bytes += static_cast<std::uint64_t>(got);
  return SendFileChunk{offset, chunk};
}

std::uint64_t SendFileSession::complete() {
  return finish(hex_encode(hasher_->finish()), total_);
}

std::uint64_t SendFileSession::complete_skipped() {
  return finish(kEmptySha256, entry_.size);
}

std::uint64_t SendFileSession::finish(const std::string& sha256, std::uint64_t total) {
  Message end{"end", {{"sha256", sha256}}};
  send_tagged_text_timed(control_, cipher_, StreamTag::FileEnd, encode_message(end), timing_);
  reporter_.file_complete(entry_.relative, total, false);
  return total;
}

ReceiveFileSession::ReceiveFileSession(Message header, std::filesystem::path output_dir,
                                       ConflictPolicy conflict_policy, const ReceivePlan* receive_plan,
                                       TcpSocket& control, StreamCipher& cipher, ProgressReporter& reporter,
                                       Bytes& prefix_buffer)
    : header_(std::move(header)),
      control_(control),
      cipher_(cipher),
      reporter_(reporter) {
  relative_ = header_.get("path");
  if (relative_.empty()) throw KikoError("file header missing path");
  declared_size_ = header_.get_u64("size", 0);
  use_zstd_ = header_.get("compress", "zstd") != "none";

  const auto* planned = find_receive_plan_entry(receive_plan, relative_);
  if (receive_plan != nullptr && planned == nullptr) {
    throw KikoError("file header was not listed in manifest: " + relative_);
  }
  if (planned != nullptr) validate_receive_plan_header(*planned, header_, relative_, declared_size_);
  current_path_ = planned != nullptr ? planned->target_path : safe_join(output_dir, relative_);

  if (is_symlink_header(header_)) {
    symlink_target_ = header_.get("target");
    validate_safe_symlink_target(relative_, symlink_target_);
    if (planned != nullptr && planned->action == ReceivePlanAction::Skip) {
      reporter_.status("skipped existing " + relative_);
      skip_symlink_ = true;
    } else if (planned != nullptr && planned->action == ReceivePlanAction::Rename) {
      report_renamed_conflict(relative_, current_path_, reporter_);
    } else if (planned == nullptr && conflict_policy == ConflictPolicy::Skip &&
               path_exists_no_follow(current_path_)) {
      reporter_.status("skipped existing " + relative_);
      skip_symlink_ = true;
    } else if (planned == nullptr && conflict_policy == ConflictPolicy::Rename &&
               path_exists_no_follow(current_path_)) {
      current_path_ = unique_conflict_path(current_path_);
      report_renamed_conflict(relative_, current_path_, reporter_);
    }
    send_resume(control_, cipher_, 0);
    (void)recv_resume_ack(control_, cipher_, 0, 0, relative_);
    reporter_.file_start(relative_, 0);
    action_ = ReceiveFileAction::Marker;
    symlink_marker_ = true;
    return;
  }

  if (is_dir_header(relative_, declared_size_)) {
    std::filesystem::create_directories(current_path_);
    apply_file_metadata(current_path_, header_);
    send_resume(control_, cipher_, 0);
    (void)recv_resume_ack(control_, cipher_, 0, 0, relative_);
    reporter_.file_start(relative_, 0);
    action_ = ReceiveFileAction::Marker;
    return;
  }

  if ((planned != nullptr && planned->action == ReceivePlanAction::Skip) ||
      (planned == nullptr && conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path_))) {
    reporter_.status("skipped existing " + relative_);
    send_resume(control_, cipher_, declared_size_, {}, true);
    const auto accepted = recv_resume_ack(control_, cipher_, declared_size_, declared_size_, relative_);
    if (accepted != declared_size_) throw KikoError("sender rejected conflict skip for " + relative_);
    reporter_.file_start(relative_, declared_size_);
    reporter_.file_advance(declared_size_);
    action_ = ReceiveFileAction::SkipPayload;
    return;
  }

  if (try_skip_existing_duplicate(control_, cipher_, header_, current_path_, relative_, declared_size_, reporter_)) {
    action_ = ReceiveFileAction::SkipPayload;
    return;
  }

  if (planned != nullptr && planned->action == ReceivePlanAction::Rename) {
    report_renamed_conflict(relative_, current_path_, reporter_);
  } else if (planned == nullptr && conflict_policy == ConflictPolicy::Rename &&
             path_exists_no_follow(current_path_)) {
    current_path_ = unique_conflict_path(current_path_);
    report_renamed_conflict(relative_, current_path_, reporter_);
  }

  if (current_path_.has_parent_path()) std::filesystem::create_directories(current_path_.parent_path());
  part_path_ = part_path_for(current_path_);

  auto have = resumable_part_size(part_path_, declared_size_);
  hasher_.emplace();
  std::string prefix_sha256;
  if (have > 0 && !hash_existing_part_prefix(part_path_, have, prefix_buffer, *hasher_, &prefix_sha256)) {
    have = 0;
    prefix_sha256.clear();
    hasher_.emplace();
  }

  send_resume(control_, cipher_, have, prefix_sha256);
  const auto accepted = recv_resume_ack(control_, cipher_, have, declared_size_, relative_);
  if (accepted != have) {
    reporter_.status("resume rejected, restarting " + relative_);
    have = accepted;
    hasher_.emplace();
    if (have > 0 && !hash_existing_part_prefix(part_path_, have, prefix_buffer, *hasher_)) {
      throw KikoError("partial file changed during resume setup: " + relative_);
    }
    std::error_code truncate_ec;
    std::filesystem::resize_file(part_path_, have, truncate_ec);
    if (truncate_ec) throw KikoError("failed to truncate partial file for " + relative_);
  }

  resume_offset_ = have;
  reporter_.file_start(relative_, declared_size_);
  if (resume_offset_ > 0) {
    reporter_.file_resume(relative_, resume_offset_, declared_size_);
    reporter_.file_advance(resume_offset_);
  }
  action_ = ReceiveFileAction::ReceivePayload;
}

void ReceiveFileSession::complete_marker() {
  if (symlink_marker_ && !skip_symlink_) {
    create_safe_symlink(current_path_, relative_, symlink_target_);
  }
  reporter_.file_complete(relative_, 0, false);
}

std::uint64_t ReceiveFileSession::complete_skipped() {
  reporter_.file_complete(relative_, declared_size_, true);
  return declared_size_;
}

std::uint64_t ReceiveFileSession::complete_received(const std::string& expected_sha256,
                                                    std::uint64_t received_size,
                                                    const std::string& actual_sha256,
                                                    Bytes& verify_buffer) {
  if (actual_sha256.empty()) {
    verify_part_file_digest(part_path_, relative_, declared_size_, expected_sha256, verify_buffer);
  } else {
    verify_received_digest(part_path_, relative_, received_size, declared_size_, expected_sha256, actual_sha256);
  }
  finalize_part_file(part_path_, current_path_);
  apply_file_metadata(current_path_, header_);
  reporter_.file_complete(relative_, declared_size_, true);
  return declared_size_;
}

Sha256Hasher& ReceiveFileSession::hasher() {
  if (!hasher_) throw KikoError("receive file session has no payload hasher");
  return *hasher_;
}

}  // namespace kiko::detail
