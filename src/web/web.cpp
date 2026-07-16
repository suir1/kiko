#include "web.hpp"

#include "core/qrcode_print.hpp"
#include "diagnostics/doctor.hpp"
#include "note/notepad.hpp"
#include "platform/path_browser.hpp"
#include "transfer/transfer.hpp"
#include "web_assets.hpp"
#include "web_job.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
constexpr std::size_t kMaxQrTextBytes = 1200;
constexpr int kDefaultPairTimeoutSec = static_cast<int>(kDefaultPairTimeout.count());

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

std::string defaulted_relay(const json& body, const WebOptions& options) {
  const auto relay = json_string(body, "relay");
  return relay.empty() ? options.relay.to_string() : relay;
}

void apply_relay_pass(const json& body, const WebOptions& options, std::optional<std::string>& out) {
  const auto relay_pass = json_string(body, "relay_pass");
  out = relay_pass.empty() ? options.relay_pass : std::optional<std::string>(relay_pass);
}

void apply_peer_connection_json(PeerConnectionOptions& config, const json& body, const WebOptions& options) {
  config.relay = parse_endpoint(defaulted_relay(body, options), 9000);
  config.no_direct = json_bool(body, "no_direct");
  config.lan_discover = !json_bool(body, "no_lan");
  config.disable_local = json_bool(body, "no_local");
  config.only_local = json_bool(body, "local");
  config.udp_probe = json_bool(body, "udp_probe");
  config.avoid_vpn = json_bool(body, "avoid_vpn");
  config.pair_timeout =
      std::chrono::seconds(std::max(1, json_int(body, "pair_timeout", kDefaultPairTimeoutSec)));
  if (const auto proxy = json_string(body, "proxy"); !proxy.empty()) config.proxy = parse_proxy_url(proxy);
  config.bind_interface = json_string(body, "bind_interface");
  if (const auto manual_ip = json_string(body, "ip"); !manual_ip.empty()) config.manual_ip = manual_ip;
  apply_relay_pass(body, options, config.relay_pass);
}

SendConfig parse_send_config(const json& body, const WebOptions& options) {
  SendConfig config;
  apply_peer_connection_json(config, body, options);
  config.file = json_string(body, "path");
  config.code = normalize_pairing_code(json_string(body, "code"));
  config.auto_connections = json_bool(body, "auto_connections");
  config.use_gitignore = !json_bool(body, "no_gitignore");
  config.connections = std::max(1, json_int(body, "connections", 4));
  config.auto_reconnect = !json_bool(body, "no_reconnect");
  config.reconnect_attempts = std::max(1, json_int(body, "reconnect_attempts", 3));
  return config;
}

RecvConfig parse_recv_config(const json& body, const WebOptions& options) {
  RecvConfig config;
  apply_peer_connection_json(config, body, options);
  config.code = normalize_pairing_code(json_string(body, "code"));
  config.output_dir = json_string(body, "out", ".");
  config.auto_reconnect = !json_bool(body, "no_reconnect");
  config.reconnect_attempts = std::max(1, json_int(body, "reconnect_attempts", 3));
  const auto conflict = json_string(body, "on_conflict", "overwrite");
  if (conflict == "skip") config.conflict_policy = ConflictPolicy::Skip;
  else if (conflict == "rename") config.conflict_policy = ConflictPolicy::Rename;
  else config.conflict_policy = ConflictPolicy::Overwrite;
  return config;
}

DoctorOptions parse_doctor_options(const json& body, const WebOptions& options) {
  DoctorOptions doctor;
  doctor.relay = parse_endpoint(defaulted_relay(body, options), 9000);
  doctor.udp_probe = json_bool(body, "udp_probe");
  doctor.avoid_vpn = json_bool(body, "avoid_vpn");
  doctor.bind_interface = json_string(body, "bind_interface");
  if (const auto proxy = json_string(body, "proxy"); !proxy.empty()) doctor.proxy = parse_proxy_url(proxy);
  apply_relay_pass(body, options, doctor.relay_pass);
  return doctor;
}

PeerSessionConfig parse_note_config(const json& body, const WebOptions& options) {
  PeerSessionConfig config;
  apply_peer_connection_json(config, body, options);
  config.role = json_string(body, "role", "host") == "join" ? Role::Receiver : Role::Sender;
  config.code = normalize_pairing_code(json_string(body, "code"));
  config.show_qrcode = false;
  config.app = "note";
  return config;
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
  out["note_text"] = "";
  out["note_active_pad"] = snapshot.note.active_pad;
  out["note_pads"] = json::array();
  out["note_revision"] = 0;
  for (const auto& document : snapshot.note.documents) {
    out["note_pads"].push_back(
        {{"id", document.pad_id},
         {"title", document.title.empty() ? document.pad_id : document.title},
         {"revision", document.revision}});
    if (document.pad_id == snapshot.note.active_pad) {
      out["note_text"] = document.text;
      out["note_revision"] = document.revision;
    }
  }
  out["note_local_revision"] = snapshot.note.latest_local_revision;
  out["note_acked_revision"] = snapshot.note.last_acked_revision;
  out["note_connected"] = snapshot.note_connected;
  out["note_synced"] = snapshot.note.synced;
  out["elapsed_ms"] = elapsed;
  out["logs"] = snapshot.logs;
  if (snapshot.overall_done > 0 && elapsed > 0) {
    out["average_bytes_per_sec"] = snapshot.overall_done * 1000 / elapsed;
  } else {
    out["average_bytes_per_sec"] = 0;
  }
  return out;
}

std::string modified_ms_string(const PathBrowserEntry& entry) {
  if (!entry.has_modified) return {};
  const auto since_epoch = entry.modified.time_since_epoch();
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count());
}

json directory_to_json(const std::filesystem::path& path, const std::vector<PathBrowserEntry>& entries) {
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

PathPickMode parse_pick_mode(const std::string& value) {
  return value == "dir" ? PathPickMode::DirectoryOnly : PathPickMode::FileOrDirectory;
}

PathBrowserSort parse_browser_sort(const std::string& value) {
  return value == "modified" ? PathBrowserSort::ModifiedDesc : PathBrowserSort::Name;
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
      : options_(std::move(options)), token_(std::move(token)) {}

  int run() {
    if (!web_listen_is_loopback(options_.listen)) {
      throw KikoError("kiko web only supports loopback listen addresses in this version");
    }

    auto listener = TcpListener::bind(options_.listen);
    const auto local = listener.local_endpoint();
    url_ = "http://" +
           (ip_address_family(local.host) == IpAddressFamily::IPv6 ? ("[" + local.host + "]") : local.host) + ":" +
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
      const auto absolute = normalize_browser_directory(std::filesystem::path(path));
      auto entries = filter_browser_entries(list_browser_directory(absolute, mode, sort), filter);
      send_json(socket, 200, "OK", directory_to_json(absolute, entries));
      return;
    }
    if (req.method == "POST" && req.path == "/api/job/cancel") {
      jobs_.cancel();
      send_json(socket, 200, "OK", json{{"ok", true}});
      return;
    }
    if (req.method == "POST" && req.path == "/api/qr") {
      const auto body = parse_body_json(req);
      const auto text = json_string(body, "text");
      if (text.empty()) {
        send_json(socket, 400, "Bad Request", error_json("QR text is empty"));
        return;
      }
      if (text.size() > kMaxQrTextBytes) {
        send_json(socket, 400, "Bad Request", error_json("QR text exceeds 1200 byte limit"));
        return;
      }
      const auto svg = qrcode_svg(text);
      if (!svg) {
        send_json(socket, 400, "Bad Request", error_json("QR code is unavailable or text is too large"));
        return;
      }
      send_json(socket, 200, "OK", json{{"svg", *svg}});
      return;
    }
    if (req.method == "POST" && (req.path == "/api/send" || req.path == "/api/recv" || req.path == "/api/doctor")) {
      const auto body = parse_body_json(req);
      std::string error;
      bool ok = false;
      try {
        if (req.path == "/api/send") ok = jobs_.start_send(parse_send_config(body, options_), error);
        if (req.path == "/api/recv") ok = jobs_.start_recv(parse_recv_config(body, options_), error);
        if (req.path == "/api/doctor") ok = jobs_.start_doctor(parse_doctor_options(body, options_), error);
      } catch (const std::exception& e) {
        error = e.what();
      }
      if (!ok) {
        const int status = error.find("already running") != std::string::npos ? 409 : 400;
        send_json(socket, status, status == 409 ? "Conflict" : "Bad Request", error_json(error));
        return;
      }
      send_json(socket, 200, "OK", json{{"ok", true}});
      return;
    }
    if (req.method == "POST" &&
        (req.path == "/api/note/start" || req.path == "/api/note/update" || req.path == "/api/note/clear" ||
         req.path == "/api/note/pad/create" || req.path == "/api/note/pad/select")) {
      const auto body = parse_body_json(req);
      std::string error;
      bool ok = false;
      try {
        if (req.path == "/api/note/start") ok = jobs_.start_note(parse_note_config(body, options_), error);
        if (req.path == "/api/note/update") ok = jobs_.update_note(json_string(body, "text"), error);
        if (req.path == "/api/note/pad/select") {
          ok = jobs_.select_note_pad(json_string(body, "pad_id"), error);
        }
      } catch (const std::exception& e) {
        error = e.what();
      }
      if (req.path == "/api/note/clear") ok = jobs_.clear_note(error);
      if (req.path == "/api/note/pad/create") ok = jobs_.create_note_pad(error);
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
    const auto& network = options_.user_config.network;
    out["network"] = {{"lan_discover", network.lan_discover},
                      {"no_direct", network.no_direct},
                      {"udp_probe", network.udp_probe},
                      {"avoid_vpn", network.avoid_vpn},
                      {"auto_connections", network.auto_connections},
                      {"connections", network.connections},
                      {"proxy_url", network.proxy_url},
                      {"bind_interface", network.bind_interface}};
    return out;
  }

  WebOptions options_;
  std::string token_;
  WebJobStore jobs_;
  std::string url_;
};

}  // namespace

bool web_listen_is_loopback(const Endpoint& endpoint) {
  return is_loopback_host(endpoint.host);
}

std::string generate_web_token() {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  Bytes bytes(24);
  for (auto& b : bytes) b = static_cast<std::uint8_t>(dist(rd));
  return hex_encode(bytes);
}

int run_web_console(const WebOptions& options) {
  WebServer server(options, generate_web_token());
  return server.run();
}

}  // namespace kiko
