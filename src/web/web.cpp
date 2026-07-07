#include "web.hpp"

#include "core/cancellation.hpp"
#include "diagnostics/doctor.hpp"
#include "note/note_protocol.hpp"
#include "note/notepad.hpp"
#include "transfer/transfer.hpp"
#include "web_assets.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

namespace kiko {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr std::size_t kMaxLogLines = 120;
constexpr int kDefaultPairTimeoutSec = static_cast<int>(kDefaultPairTimeout.count());
constexpr auto kNoteReadPoll = std::chrono::milliseconds(100);
constexpr auto kNoteHelloTimeout = std::chrono::seconds(20);

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string url_decode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const auto hex = value.substr(i + 1, 2);
      char* end = nullptr;
      const auto decoded = std::strtoul(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        out.push_back(static_cast<char>(decoded));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i] == '+' ? ' ' : value[i]);
  }
  return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
  std::map<std::string, std::string> out;
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto end = query.find('&', start);
    const auto part = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!part.empty()) {
      const auto eq = part.find('=');
      const auto key = url_decode(part.substr(0, eq));
      const auto value = eq == std::string::npos ? std::string{} : url_decode(part.substr(eq + 1));
      out[key] = value;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

std::string query_value(const std::map<std::string, std::string>& query, const std::string& key,
                        const std::string& fallback = {}) {
  const auto it = query.find(key);
  return it == query.end() ? fallback : it->second;
}

std::string json_string(const json& body, const char* key, const std::string& fallback = {}) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) return fallback;
  if (!it->is_string()) return fallback;
  return it->get<std::string>();
}

bool json_bool(const json& body, const char* key, bool fallback = false) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) return fallback;
  if (!it->is_boolean()) return fallback;
  return it->get<bool>();
}

int json_int(const json& body, const char* key, int fallback) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) return fallback;
  if (!it->is_number_integer()) return fallback;
  return it->get<int>();
}

std::string route_phase_name(RoutePhase phase, const RoutePhaseDetail& detail) {
  switch (phase) {
    case RoutePhase::Rendezvous:
      return "rendezvous";
    case RoutePhase::RelayStandby:
      return "relay fallback ready";
    case RoutePhase::DirectProbing:
      return detail.relay_fallback_ready ? "direct probing (relay ready)" : "direct probing";
    case RoutePhase::RelayCommitted:
      return "relay selected";
    case RoutePhase::Securing:
      return "securing";
  }
  return "unknown";
}

void append_timing(std::string& out, const std::string& label, int value) {
  if (value < 0) return;
  if (!out.empty()) out += " ";
  out += label + "=" + std::to_string(value) + "ms";
}

std::string route_timing_text(const RouteTiming& timing) {
  std::string out;
  append_timing(out, "rendezvous", timing.rendezvous_ms);
  append_timing(out, "direct_probe", timing.direct_probe_ms);
  append_timing(out, "relay_commit", timing.relay_commit_ms);
  append_timing(out, "securing", timing.securing_ms);
  return out;
}

std::string route_outcome_text(const RouteOutcome& outcome) {
  std::string out = "control=" + outcome.control_path + " data=" + outcome.data_path;
  if (!outcome.reason.empty()) out += " (" + outcome.reason + ")";
  if (!outcome.direct_candidate_kind.empty()) out += " via " + outcome.direct_candidate_kind;
  if (!outcome.direct_candidate_endpoint.empty()) out += " " + outcome.direct_candidate_endpoint;
  if (outcome.data_path == "relay" && !outcome.direct_failure_summary.empty()) {
    out += " direct=" + outcome.direct_failure_summary;
  }
  return out;
}

std::uint64_t elapsed_ms(const WebJobSnapshot& snapshot) {
  if (snapshot.started == std::chrono::steady_clock::time_point{}) return 0;
  const auto end = snapshot.ended.value_or(std::chrono::steady_clock::now());
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - snapshot.started).count());
}

json job_to_json(const WebJobSnapshot& snapshot) {
  json out;
  const auto elapsed = elapsed_ms(snapshot);
  out["kind"] = snapshot.kind;
  out["running"] = snapshot.running;
  out["finished"] = snapshot.finished;
  out["failed"] = snapshot.failed;
  out["canceled"] = snapshot.canceled;
  out["terminal"] = !snapshot.running && (snapshot.finished || snapshot.failed || snapshot.canceled);
  out["error"] = snapshot.error;
  out["activity"] = snapshot.activity;
  out["code"] = snapshot.code;
  out["current_file"] = snapshot.current_file;
  out["current_done"] = snapshot.current_done;
  out["current_size"] = snapshot.current_size;
  out["overall_done"] = snapshot.overall_done;
  out["overall_total"] = snapshot.overall_total;
  out["files_done"] = snapshot.files_done;
  out["files_total"] = snapshot.files_total;
  out["route_phase"] = snapshot.route_phase;
  out["route_summary"] = snapshot.route_summary;
  out["route_timing"] = snapshot.route_timing;
  out["doctor_json"] = snapshot.doctor_json;
  out["doctor_summary"] = snapshot.doctor_summary;
  out["note_text"] = snapshot.note_text;
  out["note_revision"] = snapshot.note_revision;
  out["note_local_revision"] = snapshot.note_local_revision;
  out["note_acked_revision"] = snapshot.note_acked_revision;
  out["note_connected"] = snapshot.note_connected;
  out["note_synced"] =
      snapshot.note_local_revision > 0 && snapshot.note_acked_revision >= snapshot.note_local_revision;
  out["elapsed_ms"] = elapsed;
  out["logs"] = snapshot.logs;
  if (snapshot.overall_done > 0 && elapsed > 0) {
    out["average_bytes_per_sec"] = snapshot.overall_done * 1000 / elapsed;
  } else {
    out["average_bytes_per_sec"] = 0;
  }
  return out;
}

std::string modified_ms_string(const WebDirectoryEntry& entry) {
  if (!entry.has_modified) return {};
  const auto since_epoch = entry.modified.time_since_epoch();
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count());
}

json directory_to_json(const std::filesystem::path& path, const std::vector<WebDirectoryEntry>& entries) {
  json out;
  out["path"] = path.string();
  out["entries"] = json::array();
  for (const auto& entry : entries) {
    out["entries"].push_back({{"label", entry.label},
                              {"path", entry.path.string()},
                              {"is_dir", entry.is_dir},
                              {"selectable", entry.selectable},
                              {"parent", entry.parent},
                              {"select_here", entry.select_here},
                              {"modified", modified_ms_string(entry)}});
  }
  return out;
}

void add_shortcut(json& shortcuts, const std::string& label, const std::filesystem::path& path) {
  if (label.empty() || path.empty()) return;
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec || !std::filesystem::is_directory(absolute, ec) || ec) return;
  shortcuts.push_back({{"label", label}, {"path", absolute.string()}});
}

json browser_shortcuts(const UserConfig& config) {
  json shortcuts = json::array();
  if (const char* home = std::getenv("HOME")) {
    const std::filesystem::path home_path(home);
    add_shortcut(shortcuts, "Home", home_path);
    add_shortcut(shortcuts, "Desktop", home_path / "Desktop");
    add_shortcut(shortcuts, "Downloads", home_path / "Downloads");
  }
#ifdef _WIN32
  if (const char* profile = std::getenv("USERPROFILE")) {
    const std::filesystem::path profile_path(profile);
    add_shortcut(shortcuts, "Home", profile_path);
    add_shortcut(shortcuts, "Desktop", profile_path / "Desktop");
    add_shortcut(shortcuts, "Downloads", profile_path / "Downloads");
  }
#endif
  add_shortcut(shortcuts, "Current", std::filesystem::current_path());
  if (!config.last_send_path.empty()) {
    std::error_code ec;
    auto path = std::filesystem::absolute(config.last_send_path, ec);
    if (!ec && !std::filesystem::is_directory(path, ec)) path = path.parent_path();
    add_shortcut(shortcuts, "Last send", path);
  }
  if (!config.last_recv_out_dir.empty()) add_shortcut(shortcuts, "Last receive", config.last_recv_out_dir);
  return shortcuts;
}

}  // namespace

struct WebNoteRuntime {
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_ptr<TcpSocket> channel;
  std::unique_ptr<StreamCipher> cipher;
  std::deque<NoteFrame> outgoing;
  NoteDocument document;
  std::string error;
  std::atomic_bool done{false};
};

void close_web_note_runtime(WebNoteRuntime& runtime) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.done.store(true);
  if (runtime.channel) runtime.channel->close();
  runtime.changed.notify_all();
}

std::optional<NoteFrame> recv_web_note_interruptible(TcpSocket& channel, StreamCipher& cipher,
                                                     const WebNoteRuntime& runtime,
                                                     const TransferCancellation& cancellation) {
  while (!runtime.done.load() && !cancellation.requested()) {
    auto frame = recv_note_frame_timeout(channel, cipher, kNoteReadPoll, cancellation.flag());
    if (frame) return frame;
    if (!channel.valid()) return std::nullopt;
  }
  return std::nullopt;
}

class WebJobStore {
 public:
  explicit WebJobStore(WebOptions options) : options_(std::move(options)) {}

  [[nodiscard]] WebJobSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  void append_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (line.empty()) return;
    logs_.push_back(line);
    while (logs_.size() > kMaxLogLines) logs_.pop_front();
    state_.logs.assign(logs_.begin(), logs_.end());
  }

  void update(const std::function<void(WebJobSnapshot&)>& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    fn(state_);
    state_.logs.assign(logs_.begin(), logs_.end());
  }

  [[nodiscard]] bool start_send(const json& body, std::string& error) {
    try {
      join_finished_worker();
      SendConfig config;
      config.file = json_string(body, "path");
      if (config.file.empty()) throw KikoError("send path is required");
      config.relay = parse_endpoint(defaulted_relay(body), 9000);
      config.code = json_string(body, "code");
      config.no_direct = json_bool(body, "no_direct");
      config.udp_probe = json_bool(body, "udp_probe");
      config.avoid_vpn = json_bool(body, "avoid_vpn");
      config.auto_connections = json_bool(body, "auto_connections");
      config.use_gitignore = !json_bool(body, "no_gitignore");
      config.connections = std::max(1, json_int(body, "connections", 4));
      config.auto_reconnect = !json_bool(body, "no_reconnect");
      config.reconnect_attempts = std::max(1, json_int(body, "reconnect_attempts", 3));
      config.pair_timeout = std::chrono::seconds(std::max(1, json_int(body, "pair_timeout", kDefaultPairTimeoutSec)));
      if (const auto proxy = json_string(body, "proxy"); !proxy.empty()) config.proxy = parse_proxy_url(proxy);
      config.bind_interface = json_string(body, "bind_interface");
      if (const auto manual_ip = json_string(body, "ip"); !manual_ip.empty()) config.manual_ip = manual_ip;
      apply_relay_pass(body, config.relay_pass);

      auto cancellation = std::make_shared<TransferCancellation>();
      config.cancellation = cancellation;
      if (!begin_task("send", cancellation, error)) return false;

      worker_ = std::thread([this, config = std::move(config), cancellation = std::move(cancellation)]() mutable {
        WebReporter reporter(*this);
        try {
          const int rc = run_send(config, reporter);
          if (rc == 0) {
            finish_success("send complete");
          } else if (cancellation->requested()) {
            finish_canceled();
          } else {
            finish_failed("send exited with code " + std::to_string(rc));
          }
        } catch (const std::exception& e) {
          cancellation->requested() ? finish_canceled() : finish_failed(e.what());
        }
      });
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  [[nodiscard]] bool start_recv(const json& body, std::string& error) {
    try {
      join_finished_worker();
      RecvConfig config;
      config.code = json_string(body, "code");
      if (config.code.empty()) throw KikoError("receive code is required");
      config.output_dir = json_string(body, "out", ".");
      config.relay = parse_endpoint(defaulted_relay(body), 9000);
      config.no_direct = json_bool(body, "no_direct");
      config.lan_discover = !json_bool(body, "no_lan");
      config.disable_local = json_bool(body, "no_local");
      config.only_local = json_bool(body, "local");
      config.udp_probe = json_bool(body, "udp_probe");
      config.avoid_vpn = json_bool(body, "avoid_vpn");
      config.auto_reconnect = !json_bool(body, "no_reconnect");
      config.reconnect_attempts = std::max(1, json_int(body, "reconnect_attempts", 3));
      config.pair_timeout = std::chrono::seconds(std::max(1, json_int(body, "pair_timeout", kDefaultPairTimeoutSec)));
      const auto conflict = json_string(body, "on_conflict", "overwrite");
      if (conflict == "skip") config.conflict_policy = ConflictPolicy::Skip;
      else if (conflict == "rename") config.conflict_policy = ConflictPolicy::Rename;
      else config.conflict_policy = ConflictPolicy::Overwrite;
      if (const auto proxy = json_string(body, "proxy"); !proxy.empty()) config.proxy = parse_proxy_url(proxy);
      config.bind_interface = json_string(body, "bind_interface");
      if (const auto manual_ip = json_string(body, "ip"); !manual_ip.empty()) config.manual_ip = manual_ip;
      apply_relay_pass(body, config.relay_pass);

      auto cancellation = std::make_shared<TransferCancellation>();
      config.cancellation = cancellation;
      if (!begin_task("recv", cancellation, error)) return false;

      worker_ = std::thread([this, config = std::move(config), cancellation = std::move(cancellation)]() mutable {
        WebReporter reporter(*this);
        try {
          const int rc = run_recv(config, reporter);
          if (rc == 0) {
            finish_success("receive complete");
          } else if (cancellation->requested()) {
            finish_canceled();
          } else {
            finish_failed("receive exited with code " + std::to_string(rc));
          }
        } catch (const std::exception& e) {
          cancellation->requested() ? finish_canceled() : finish_failed(e.what());
        }
      });
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  [[nodiscard]] bool start_doctor(const json& body, std::string& error) {
    try {
      join_finished_worker();
      DoctorOptions opts;
      opts.relay = parse_endpoint(defaulted_relay(body), 9000);
      opts.udp_probe = json_bool(body, "udp_probe");
      opts.avoid_vpn = json_bool(body, "avoid_vpn");
      opts.bind_interface = json_string(body, "bind_interface");
      if (const auto proxy = json_string(body, "proxy"); !proxy.empty()) opts.proxy = parse_proxy_url(proxy);
      apply_relay_pass(body, opts.relay_pass);

      auto cancellation = std::make_shared<TransferCancellation>();
      if (!begin_task("doctor", cancellation, error)) return false;

      worker_ = std::thread([this, opts = std::move(opts), cancellation = std::move(cancellation)]() mutable {
        try {
          append_log("doctor started");
          const auto report = run_doctor(opts);
          if (cancellation->requested()) {
            finish_canceled();
            return;
          }
          const auto report_json = doctor_report_to_json(report);
          update([&](WebJobSnapshot& state) {
            state.doctor_json = report_json;
            state.doctor_summary = report.diagnosis;
            state.activity = "doctor complete";
            state.finished = true;
          });
          append_log(report.diagnosis);
          finish_success("doctor complete");
        } catch (const std::exception& e) {
          cancellation->requested() ? finish_canceled() : finish_failed(e.what());
        }
      });
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  [[nodiscard]] bool start_note(const json& body, std::string& error) {
    try {
      join_finished_worker();
      NoteConfig config;
      const auto role = json_string(body, "role", "host");
      config.role = role == "join" ? Role::Receiver : Role::Sender;
      config.code = json_string(body, "code");
      if (config.role == Role::Receiver && config.code.empty()) throw KikoError("note code is required");
      config.relay = parse_endpoint(defaulted_relay(body), 9000);
      config.no_direct = json_bool(body, "no_direct");
      config.lan_discover = !json_bool(body, "no_lan");
      config.disable_local = json_bool(body, "no_local");
      config.only_local = json_bool(body, "local");
      config.udp_probe = json_bool(body, "udp_probe");
      config.avoid_vpn = json_bool(body, "avoid_vpn");
      config.pair_timeout = std::chrono::seconds(std::max(1, json_int(body, "pair_timeout", kDefaultPairTimeoutSec)));
      config.show_qrcode = false;
      config.app = "note";
      if (const auto proxy = json_string(body, "proxy"); !proxy.empty()) config.proxy = parse_proxy_url(proxy);
      config.bind_interface = json_string(body, "bind_interface");
      if (const auto manual_ip = json_string(body, "ip"); !manual_ip.empty()) config.manual_ip = manual_ip;
      apply_relay_pass(body, config.relay_pass);

      auto cancellation = std::make_shared<TransferCancellation>();
      config.cancellation = cancellation;
      auto runtime = std::make_shared<WebNoteRuntime>();
      if (!begin_task("note", cancellation, error)) return false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        note_runtime_ = runtime;
        state_.activity = config.role == Role::Sender ? "hosting notepad" : "joining notepad";
        state_.code = config.code;
      }

      worker_ = std::thread([this, config = std::move(config), cancellation = std::move(cancellation),
                             runtime = std::move(runtime)]() mutable {
        WebReporter reporter(*this);
        std::thread sender([this, runtime, cancellation] {
          try {
            while (true) {
              NoteFrame frame;
              TcpSocket* channel = nullptr;
              StreamCipher* cipher = nullptr;
              {
                std::unique_lock<std::mutex> lock(runtime->mutex);
                runtime->changed.wait(lock, [&] {
                  return runtime->done.load() ||
                         (runtime->channel && runtime->cipher && !runtime->outgoing.empty());
                });
                if (runtime->done.load() && runtime->outgoing.empty()) break;
                if (!runtime->channel || !runtime->cipher || runtime->outgoing.empty()) continue;
                frame = std::move(runtime->outgoing.front());
                runtime->outgoing.pop_front();
                channel = runtime->channel.get();
                cipher = runtime->cipher.get();
              }
              send_note_frame(*channel, *cipher, frame);
              if (frame.type == NoteFrameType::Update || frame.type == NoteFrameType::Clear) {
                update([&](WebJobSnapshot& state) {
                  state.note_local_revision = std::max(state.note_local_revision, frame.revision);
                  state.activity =
                      state.note_acked_revision >= state.note_local_revision ? "notepad synced" : "notepad sent";
                });
              }
            }
          } catch (const std::exception& e) {
            if (!runtime->done.load() && !cancellation->requested()) {
              {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                runtime->error = e.what();
              }
              append_log(std::string("note send error: ") + e.what());
              cancellation->request();
            }
            close_web_note_runtime(*runtime);
          }
        });

        try {
          auto session = open_peer_session(config, reporter);
          auto channel = std::make_unique<TcpSocket>(std::move(session.encrypted.channel));
          auto cipher = std::make_unique<StreamCipher>(session.encrypted.key, config.role == Role::Sender);
          send_note_frame(*channel, *cipher, make_note_hello());
          append_log("notepad hello sent");
          auto hello = recv_note_frame_timeout(*channel, *cipher, kNoteHelloTimeout, cancellation->flag());
          if (!hello || hello->type != NoteFrameType::Hello) throw KikoError("note peer did not send hello");
          append_log("notepad peer hello received");

          {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            runtime->channel = std::move(channel);
            runtime->cipher = std::move(cipher);
          }
          runtime->changed.notify_all();
          update([&](WebJobSnapshot& state) {
            state.note_connected = true;
            state.code = session.code;
            state.route_summary = session.outcome.data_path;
            state.activity = "notepad connected";
          });
          append_log("notepad connected via " + session.outcome.data_path);

          while (!runtime->done.load()) {
            TcpSocket* active_channel = nullptr;
            StreamCipher* active_cipher = nullptr;
            {
              std::lock_guard<std::mutex> lock(runtime->mutex);
              active_channel = runtime->channel.get();
              active_cipher = runtime->cipher.get();
            }
            if (!active_channel || !active_cipher) break;
            auto incoming =
                recv_web_note_interruptible(*active_channel, *active_cipher, *runtime, *cancellation);
            if (!incoming) break;
            if (incoming->type == NoteFrameType::Bye) break;
            if (incoming->type == NoteFrameType::Ack) {
              update([&](WebJobSnapshot& state) {
                state.note_acked_revision = std::max(state.note_acked_revision, incoming->revision);
                state.activity =
                    state.note_acked_revision >= state.note_local_revision ? "notepad synced" : "notepad sent";
              });
              continue;
            }
            if (incoming->type != NoteFrameType::Update && incoming->type != NoteFrameType::Clear) continue;

            bool applied = false;
            std::string text;
            std::uint64_t revision = 0;
            {
              std::lock_guard<std::mutex> lock(runtime->mutex);
              applied = apply_note_update(runtime->document, *incoming);
              if (applied) {
                text = runtime->document.text;
                revision = runtime->document.revision;
                runtime->outgoing.push_back(make_note_ack(incoming->revision));
              }
            }
            if (applied) {
              runtime->changed.notify_one();
              update([&](WebJobSnapshot& state) {
                state.note_text = text;
                state.note_revision = revision;
                state.activity = "remote note update";
              });
            }
          }

          std::string send_error;
          {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            send_error = runtime->error;
          }
          if (!send_error.empty()) {
            finish_failed(send_error);
          } else if (cancellation->requested() || runtime->done.load()) {
            finish_canceled();
          } else {
            finish_success("notepad closed");
          }
        } catch (const std::exception& e) {
          if (cancellation->requested() || runtime->done.load()) {
            finish_canceled();
          } else {
            finish_failed(e.what());
          }
        }
        close_web_note_runtime(*runtime);
        if (sender.joinable()) sender.join();
      });
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  [[nodiscard]] bool update_note(const json& body, std::string& error) {
    const auto text = json_string(body, "text");
    if (text.size() > kNoteMaxBytes) {
      error = "note text exceeds 1 MiB limit";
      return false;
    }
    auto runtime = current_note_runtime(error);
    if (!runtime) return false;

    NoteFrame frame;
    std::uint64_t revision = 0;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->done.load()) {
        error = "notepad is closed";
        return false;
      }
      frame = make_note_update(runtime->document.revision + 1, text);
      (void)apply_note_update(runtime->document, frame);
      revision = frame.revision;
      runtime->outgoing.push_back(frame);
    }
    runtime->changed.notify_one();
    update([&](WebJobSnapshot& state) {
      state.note_text = text;
      state.note_revision = revision;
      state.note_local_revision = std::max(state.note_local_revision, revision);
      state.activity = "notepad syncing";
    });
    return true;
  }

  [[nodiscard]] bool clear_note(std::string& error) {
    auto runtime = current_note_runtime(error);
    if (!runtime) return false;

    std::uint64_t revision = 0;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->done.load()) {
        error = "notepad is closed";
        return false;
      }
      auto frame = make_note_clear(runtime->document.revision + 1);
      (void)apply_note_update(runtime->document, frame);
      revision = frame.revision;
      runtime->outgoing.push_back(std::move(frame));
    }
    runtime->changed.notify_one();
    update([&](WebJobSnapshot& state) {
      state.note_text.clear();
      state.note_revision = revision;
      state.note_local_revision = std::max(state.note_local_revision, revision);
      state.activity = "notepad syncing";
    });
    return true;
  }

  void cancel() {
    std::shared_ptr<TransferCancellation> cancellation;
    std::shared_ptr<WebNoteRuntime> note_runtime;
    bool should_log = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancellation = cancellation_;
      note_runtime = note_runtime_;
      if (state_.running) {
        should_log = state_.activity != "cancel requested";
        state_.activity = "cancel requested";
      }
    }
    if (should_log) append_log("cancel requested");
    if (cancellation) cancellation->request();
    if (note_runtime) close_web_note_runtime(*note_runtime);
  }

  void join_finished_worker() {
    std::thread old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_.running || !worker_.joinable()) return;
      old = std::move(worker_);
    }
    if (old.joinable()) old.join();
  }

 private:
  std::string defaulted_relay(const json& body) const {
    const auto relay = json_string(body, "relay");
    return relay.empty() ? options_.relay.to_string() : relay;
  }

  void apply_relay_pass(const json& body, std::optional<std::string>& out) const {
    const auto relay_pass = json_string(body, "relay_pass");
    if (!relay_pass.empty()) out = relay_pass;
    else out = options_.relay_pass;
  }

  bool begin_task(const std::string& kind, std::shared_ptr<TransferCancellation> cancellation, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.running) {
      error = "another kiko web task is already running";
      return false;
    }
    note_runtime_.reset();
    logs_.clear();
    state_ = {};
    state_.kind = kind;
    state_.running = true;
    state_.activity = "starting";
    state_.started = std::chrono::steady_clock::now();
    state_.logs = {};
    cancellation_ = std::move(cancellation);
    return true;
  }

  std::shared_ptr<WebNoteRuntime> current_note_runtime(std::string& error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.running || state_.kind != "note" || !note_runtime_) {
      error = "notepad is not running";
      return {};
    }
    return note_runtime_;
  }

  void finish_success(const std::string& activity) {
    update([&](WebJobSnapshot& state) {
      state.running = false;
      if (!state.failed && !state.canceled) {
        state.finished = true;
        state.activity = activity;
      }
      state.ended = std::chrono::steady_clock::now();
    });
  }

  void finish_failed(const std::string& error) {
    append_log("error: " + error);
    update([&](WebJobSnapshot& state) {
      state.running = false;
      state.finished = true;
      state.failed = true;
      state.canceled = false;
      state.error = error;
      state.activity = "failed";
      state.ended = std::chrono::steady_clock::now();
    });
  }

  void finish_canceled() {
    append_log("canceled");
    update([](WebJobSnapshot& state) {
      state.running = false;
      state.finished = true;
      state.failed = false;
      state.canceled = true;
      state.error.clear();
      state.activity = "canceled";
      state.ended = std::chrono::steady_clock::now();
    });
  }

  WebOptions options_;
  mutable std::mutex mutex_;
  WebJobSnapshot state_;
  std::deque<std::string> logs_;
  std::shared_ptr<TransferCancellation> cancellation_;
  std::shared_ptr<WebNoteRuntime> note_runtime_;
  std::thread worker_;
};

namespace {

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> headers;
  std::string body;
};

std::optional<HttpRequest> read_http_request(TcpSocket& socket) {
  std::string data;
  asio::error_code ec;
  asio::read_until(socket.asio_socket(), asio::dynamic_buffer(data), "\r\n\r\n", ec);
  if (ec) return std::nullopt;

  const auto header_end = data.find("\r\n\r\n");
  if (header_end == std::string::npos) return std::nullopt;

  std::istringstream headers(data.substr(0, header_end));
  std::string request_line;
  std::getline(headers, request_line);
  if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

  std::istringstream first(request_line);
  std::string target;
  HttpRequest req;
  first >> req.method >> target;
  if (req.method.empty() || target.empty()) return std::nullopt;

  const auto query_pos = target.find('?');
  req.path = query_pos == std::string::npos ? target : target.substr(0, query_pos);
  req.query = query_pos == std::string::npos ? std::map<std::string, std::string>{}
                                             : parse_query(target.substr(query_pos + 1));

  std::string line;
  while (std::getline(headers, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto name = lower(trim(line.substr(0, colon)));
    auto value = trim(line.substr(colon + 1));
    req.headers[name] = value;
  }

  std::size_t content_length = 0;
  if (const auto it = req.headers.find("content-length"); it != req.headers.end()) {
    content_length = static_cast<std::size_t>(std::stoull(it->second));
    if (content_length > kMaxBodyBytes) throw KikoError("request body too large");
  }

  req.body = data.substr(header_end + 4);
  if (req.body.size() < content_length) {
    asio::read(socket.asio_socket(), asio::dynamic_buffer(data),
               asio::transfer_exactly(content_length - req.body.size()), ec);
    if (ec) return std::nullopt;
    req.body = data.substr(header_end + 4);
  }
  if (req.body.size() > content_length) req.body.resize(content_length);
  return req;
}

void send_response(TcpSocket& socket, int status, const std::string& reason, const std::string& content_type,
                   const std::string& body) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  asio::error_code ignored;
  asio::write(socket.asio_socket(), asio::buffer(out.str()), ignored);
}

void send_json(TcpSocket& socket, int status, const std::string& reason, const json& body) {
  send_response(socket, status, reason, "application/json; charset=utf-8", body.dump());
}

json error_json(const std::string& error) { return json{{"error", error}}; }

bool token_ok(const HttpRequest& req, const std::string& token) {
  if (query_value(req.query, "token") == token) return true;
  const auto it = req.headers.find("x-kiko-token");
  return it != req.headers.end() && it->second == token;
}

json parse_body_json(const HttpRequest& req) {
  if (req.body.empty()) return json::object();
  try {
    return json::parse(req.body);
  } catch (const std::exception& e) {
    throw KikoError(std::string("invalid JSON body: ") + e.what());
  }
}

WebPickMode parse_pick_mode(const std::string& value) {
  return value == "dir" ? WebPickMode::DirectoryOnly : WebPickMode::FileOrDirectory;
}

WebBrowserSort parse_browser_sort(const std::string& value) {
  return value == "modified" ? WebBrowserSort::ModifiedDesc : WebBrowserSort::Name;
}

void open_browser_best_effort(const std::string& url) {
#ifdef _WIN32
  const std::string command = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  const std::string command = "open \"" + url + "\" >/dev/null 2>&1 &";
#else
  const std::string command = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
  (void)std::system(command.c_str());
}

class WebServer {
 public:
  WebServer(WebOptions options, std::string token)
      : options_(std::move(options)), token_(std::move(token)), jobs_(options_) {}

  int run() {
    if (!web_listen_is_loopback(options_.listen)) {
      throw KikoError("kiko web only supports loopback listen addresses in this version");
    }

    auto listener = TcpListener::bind(options_.listen);
    const auto local = listener.local_endpoint();
    url_ = "http://" + (is_ipv6_address(local.host) ? ("[" + local.host + "]") : local.host) + ":" +
           std::to_string(local.port) + "/?token=" + token_;
    std::cout << "kiko web listening on " << url_ << "\n" << std::flush;
    if (options_.open_browser) open_browser_best_effort(url_);

    while (true) {
      auto client = listener.accept(std::chrono::milliseconds(1000));
      jobs_.join_finished_worker();
      if (!client.valid()) continue;
      std::thread([this, client = std::move(client)]() mutable { handle_client(client); }).detach();
    }
  }

 private:
  void handle_client(TcpSocket& socket) {
    try {
      auto maybe_req = read_http_request(socket);
      if (!maybe_req) return;
      const auto& req = *maybe_req;
      if (req.path == "/" || req.path == "/index.html") {
        send_response(socket, 200, "OK", "text/html; charset=utf-8", std::string(web_index_html()));
        return;
      }
      if (req.path.rfind("/api/", 0) != 0) {
        send_json(socket, 404, "Not Found", error_json("not found"));
        return;
      }
      if (!token_ok(req, token_)) {
        send_json(socket, 401, "Unauthorized", error_json("unauthorized"));
        return;
      }
      handle_api(socket, req);
    } catch (const std::exception& e) {
      send_json(socket, 500, "Internal Server Error", error_json(e.what()));
    }
  }

  void handle_api(TcpSocket& socket, const HttpRequest& req) {
    if (req.method == "GET" && req.path == "/api/config") {
      send_json(socket, 200, "OK", config_json());
      return;
    }
    if (req.method == "GET" && req.path == "/api/job") {
      send_json(socket, 200, "OK", job_to_json(jobs_.snapshot()));
      return;
    }
    if (req.method == "GET" && req.path == "/api/fs") {
      const auto path = query_value(req.query, "path", ".");
      const auto mode = parse_pick_mode(query_value(req.query, "mode"));
      const auto sort = parse_browser_sort(query_value(req.query, "sort"));
      const auto filter = query_value(req.query, "filter");
      std::error_code ec;
      auto absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
      if (ec) throw KikoError("invalid path: " + path);
      if (!std::filesystem::is_directory(absolute, ec) || ec) {
        absolute = absolute.parent_path();
      }
      auto entries = list_web_directory(absolute, mode, sort, filter);
      send_json(socket, 200, "OK", directory_to_json(absolute, entries));
      return;
    }
    if (req.method == "POST" && req.path == "/api/job/cancel") {
      jobs_.cancel();
      send_json(socket, 200, "OK", json{{"ok", true}});
      return;
    }
    if (req.method == "POST" && (req.path == "/api/send" || req.path == "/api/recv" || req.path == "/api/doctor")) {
      const auto body = parse_body_json(req);
      std::string error;
      bool ok = false;
      if (req.path == "/api/send") ok = jobs_.start_send(body, error);
      if (req.path == "/api/recv") ok = jobs_.start_recv(body, error);
      if (req.path == "/api/doctor") ok = jobs_.start_doctor(body, error);
      if (!ok) {
        const int status = error.find("already running") != std::string::npos ? 409 : 400;
        send_json(socket, status, status == 409 ? "Conflict" : "Bad Request", error_json(error));
        return;
      }
      send_json(socket, 200, "OK", json{{"ok", true}});
      return;
    }
    if (req.method == "POST" &&
        (req.path == "/api/note/start" || req.path == "/api/note/update" || req.path == "/api/note/clear")) {
      const auto body = parse_body_json(req);
      std::string error;
      bool ok = false;
      if (req.path == "/api/note/start") ok = jobs_.start_note(body, error);
      if (req.path == "/api/note/update") ok = jobs_.update_note(body, error);
      if (req.path == "/api/note/clear") ok = jobs_.clear_note(error);
      if (!ok) {
        const int status = error.find("already running") != std::string::npos ? 409 : 400;
        send_json(socket, status, status == 409 ? "Conflict" : "Bad Request", error_json(error));
        return;
      }
      send_json(socket, 200, "OK", json{{"ok", true}});
      return;
    }
    send_json(socket, 404, "Not Found", error_json("not found"));
  }

  json config_json() const {
    json out;
    out["listen"] = url_;
    out["relay"] = options_.relay.to_string();
    out["has_relay_pass"] = options_.relay_pass.has_value();
    out["last_send_path"] = options_.user_config.last_send_path;
    out["last_recv_out_dir"] = options_.user_config.last_recv_out_dir.empty() ? "." : options_.user_config.last_recv_out_dir;
    out["shortcuts"] = browser_shortcuts(options_.user_config);
    out["network"] = {{"lan_discover", options_.user_config.lan_discover},
                      {"no_direct", options_.user_config.no_direct},
                      {"udp_probe", options_.user_config.udp_probe},
                      {"avoid_vpn", options_.user_config.avoid_vpn},
                      {"auto_connections", options_.user_config.auto_connections},
                      {"connections", options_.user_config.connections},
                      {"proxy_url", options_.user_config.proxy_url},
                      {"bind_interface", options_.user_config.bind_interface}};
    return out;
  }

  WebOptions options_;
  std::string token_;
  WebJobStore jobs_;
  std::string url_;
};

}  // namespace

bool web_listen_is_loopback(const Endpoint& endpoint) {
  if (endpoint.host == "localhost") return true;
  return ip_address_scope(endpoint.host) == IpAddressScope::Loopback;
}

std::string generate_web_token() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  Bytes bytes(24);
  for (auto& b : bytes) b = static_cast<std::uint8_t>(dist(rd));
  return hex_encode(bytes);
}

std::vector<WebDirectoryEntry> list_web_directory(const std::filesystem::path& dir, WebPickMode mode,
                                                  WebBrowserSort sort, const std::string& filter) {
  std::vector<WebDirectoryEntry> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) || ec) throw KikoError("not a directory: " + dir.string());

  auto add_modified = [](WebDirectoryEntry& entry) {
    std::error_code time_ec;
    entry.modified = std::filesystem::last_write_time(entry.path, time_ec);
    entry.has_modified = !time_ec;
  };

  if (dir.has_parent_path()) {
    WebDirectoryEntry parent;
    parent.label = "../";
    parent.path = dir.parent_path();
    parent.is_dir = true;
    parent.parent = true;
    out.push_back(std::move(parent));
  }

  std::vector<WebDirectoryEntry> dirs;
  std::vector<WebDirectoryEntry> files;
  const auto lowered_filter = lower(filter);
  for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    WebDirectoryEntry entry;
    entry.path = item.path();
    std::error_code type_ec;
    entry.is_dir = item.is_directory(type_ec);
    if (type_ec) continue;
    const auto name = item.path().filename().string();
    if (name.empty() || name == ".") continue;
    const bool matches = lowered_filter.empty() || lower(name).find(lowered_filter) != std::string::npos;
    if (!matches) continue;
    entry.label = entry.is_dir ? name + "/" : name;
    entry.selectable = entry.is_dir || mode == WebPickMode::FileOrDirectory;
    add_modified(entry);
    if (entry.is_dir) {
      dirs.push_back(std::move(entry));
    } else if (mode == WebPickMode::FileOrDirectory) {
      files.push_back(std::move(entry));
    }
  }

  auto by_name = [](const WebDirectoryEntry& a, const WebDirectoryEntry& b) { return a.label < b.label; };
  auto by_modified = [&](const WebDirectoryEntry& a, const WebDirectoryEntry& b) {
    if (a.has_modified != b.has_modified) return a.has_modified;
    if (a.has_modified && b.has_modified && a.modified != b.modified) return a.modified > b.modified;
    return by_name(a, b);
  };
  if (sort == WebBrowserSort::ModifiedDesc) {
    std::sort(dirs.begin(), dirs.end(), by_modified);
    std::sort(files.begin(), files.end(), by_modified);
  } else {
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
  }
  if (mode == WebPickMode::FileOrDirectory) {
    out.insert(out.end(), files.begin(), files.end());
    out.insert(out.end(), dirs.begin(), dirs.end());
  } else {
    out.insert(out.end(), dirs.begin(), dirs.end());
  }

  WebDirectoryEntry here;
  here.label = "[Select this folder]";
  here.path = dir;
  here.is_dir = true;
  here.selectable = true;
  here.select_here = true;
  out.push_back(std::move(here));
  return out;
}

WebReporter::WebReporter(WebJobStore& store) : store_(store) {}

void WebReporter::status(const std::string& message) {
  store_.append_log(message);
  store_.update([&](WebJobSnapshot& state) { state.activity = message; });
}

void WebReporter::connectivity_report(const std::string& report) {
  store_.append_log(report);
  store_.update([](WebJobSnapshot& state) { state.activity = "connectivity report"; });
}

void WebReporter::route_phase(RoutePhase phase, const RoutePhaseDetail& detail) {
  const auto label = route_phase_name(phase, detail);
  store_.append_log("route phase: " + label + (detail.reason.empty() ? std::string{} : " (" + detail.reason + ")"));
  store_.update([&](WebJobSnapshot& state) {
    state.route_phase = label;
    state.activity = detail.message.empty() ? label : detail.message;
  });
}

void WebReporter::route_outcome(const RouteOutcome& outcome) {
  const auto summary = route_outcome_text(outcome);
  store_.append_log("route outcome: " + summary);
  store_.update([&](WebJobSnapshot& state) {
    state.route_summary = summary;
    state.activity = outcome.data_path == "direct" ? "direct TCP selected" : "relay TCP selected";
  });
}

void WebReporter::route_timing(const RouteTiming& timing) {
  const auto text = route_timing_text(timing);
  if (text.empty()) return;
  store_.append_log("route timing: " + text);
  store_.update([&](WebJobSnapshot& state) { state.route_timing = text; });
}

void WebReporter::handshake_ok() {
  store_.append_log("handshake ok");
  store_.update([](WebJobSnapshot& state) { state.activity = "encrypted channel ready"; });
}

void WebReporter::code_ready(const std::string& code, bool show_qrcode) {
  (void)show_qrcode;
  store_.update([&](WebJobSnapshot& state) { state.code = code; });
}

void WebReporter::transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
  store_.update([&](WebJobSnapshot& state) {
    state.files_total = file_count;
    state.overall_total = total_bytes;
    state.activity = "transferring";
  });
}

void WebReporter::receive_plan(const ReceivePlanSummary& summary) {
  store_.append_log(format_receive_plan_summary(summary));
  store_.update([&](WebJobSnapshot& state) {
    state.overall_total = summary.total_bytes;
    state.activity = "receive plan ready";
  });
}

void WebReporter::file_start(const std::string& path, std::uint64_t size) {
  store_.update([&](WebJobSnapshot& state) {
    state.current_file = path;
    state.current_done = 0;
    state.current_size = size;
  });
}

void WebReporter::file_advance(std::uint64_t bytes_delta) {
  store_.update([&](WebJobSnapshot& state) {
    if (state.finished || state.failed || state.canceled) return;
    state.current_done += bytes_delta;
    state.overall_done += bytes_delta;
  });
}

void WebReporter::file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) {
  store_.append_log("resume: " + path + " from " + std::to_string(offset) + "/" + std::to_string(size));
}

void WebReporter::file_complete(const std::string& path, std::uint64_t size, bool verified) {
  (void)path;
  (void)size;
  (void)verified;
  store_.update([](WebJobSnapshot& state) { ++state.files_done; });
}

void WebReporter::transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
  store_.update([&](WebJobSnapshot& state) {
    state.files_total = file_count;
    state.files_done = file_count;
    state.overall_done = total_bytes;
    if (state.overall_total == 0) state.overall_total = total_bytes;
    state.finished = true;
    state.activity = "transfer complete";
    state.ended = std::chrono::steady_clock::now();
  });
}

void WebReporter::transfer_retry(int next_attempt, int max_attempts, const std::string& reason) {
  store_.append_log("retry " + std::to_string(next_attempt) + "/" + std::to_string(max_attempts) + ": " + reason);
  store_.update([&](WebJobSnapshot& state) {
    state.activity = "reconnecting " + std::to_string(next_attempt) + "/" + std::to_string(max_attempts);
  });
}

void WebReporter::transfer_retry_delay(int next_attempt, int max_attempts, std::chrono::milliseconds delay) {
  store_.append_log("reconnect in " + std::to_string(delay.count()) + "ms before attempt " +
                    std::to_string(next_attempt) + "/" + std::to_string(max_attempts));
}

int run_web_console(const WebOptions& options) {
  WebServer server(options, generate_web_token());
  return server.run();
}

}  // namespace kiko
