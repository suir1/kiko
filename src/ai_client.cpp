#include "ai_client.hpp"

#include "common.hpp"
#include "platform.hpp"
#include "socket.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

namespace kiko {
namespace {

struct ParsedUrl {
  bool https = false;
  std::string host;
  std::uint16_t port = 0;
  std::string path_prefix;
};

std::uint16_t parse_url_port(const std::string& text) {
  auto port = parse_u64_strict(text);
  if (!port || *port == 0 || *port > 65535) throw KikoError("invalid AI base_url port");
  return static_cast<std::uint16_t>(*port);
}

ParsedUrl parse_base_url(const std::string& base_url) {
  ParsedUrl out;
  std::string rest = base_url;
  if (rest.rfind("https://", 0) == 0) {
    out.https = true;
    rest = rest.substr(8);
  } else if (rest.rfind("http://", 0) == 0) {
    rest = rest.substr(7);
  } else {
    throw KikoError("AI base_url must start with http:// or https://");
  }
  auto slash = rest.find('/');
  const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  out.path_prefix = slash == std::string::npos ? "" : rest.substr(slash);
  if (!out.path_prefix.empty() && out.path_prefix.back() == '/') out.path_prefix.pop_back();
  if (hostport.empty()) throw KikoError("invalid AI base_url host");

  if (hostport.front() == '[') {
    auto close = hostport.find(']');
    if (close == std::string::npos) throw KikoError("invalid AI base_url host");
    out.host = hostport.substr(1, close - 1);
    if (close + 1 < hostport.size() && hostport[close + 1] == ':') {
      out.port = parse_url_port(hostport.substr(close + 2));
    } else if (close + 1 != hostport.size()) {
      throw KikoError("invalid AI base_url host");
    } else {
      out.port = out.https ? 443 : 80;
    }
  } else {
    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
      out.host = hostport.substr(0, colon);
      out.port = parse_url_port(hostport.substr(colon + 1));
    } else {
      out.host = hostport;
      out.port = out.https ? 443 : 80;
    }
  }
  if (out.host.empty()) throw KikoError("invalid AI base_url host");
  return out;
}

std::string shell_escape_single(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

AiChatResult curl_https_post(const ParsedUrl& url, const std::string& api_key, const std::string& body,
                             std::chrono::milliseconds timeout) {
  AiChatResult result;
  const auto seconds = std::max<std::int64_t>(1, timeout.count() / 1000);
  const std::string target = "https://" + url.host + ":" + std::to_string(url.port) + url.path_prefix + "/chat/completions";
  const std::string cmd = "curl -sS --max-time " + std::to_string(seconds) + " -H " + shell_escape_single("Authorization: Bearer " + api_key) +
                          " -H " + shell_escape_single("Content-Type: application/json") + " -d " + shell_escape_single(body) + " " +
                          shell_escape_single(target) + " 2>&1";

  std::array<char, 4096> buf{};
  std::string output;
  const std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) {
    result.error = "failed to invoke curl for HTTPS AI request";
    return result;
  }
  while (true) {
    auto n = fread(buf.data(), 1, buf.size(), pipe.get());
    if (n == 0) break;
    output.append(buf.data(), n);
  }
  if (output.empty()) {
    result.error = "empty response from AI endpoint";
    return result;
  }
  try {
    auto j = nlohmann::json::parse(output);
    if (j.contains("error")) {
      result.error = j["error"].dump();
      return result;
    }
    result.content = j["choices"][0]["message"]["content"].get<std::string>();
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("AI response parse failed: ") + e.what();
  }
  return result;
}

AiChatResult http_post(const ParsedUrl& url, const std::string& api_key, const std::string& body,
                       std::chrono::milliseconds timeout) {
  AiChatResult result;
  auto socket = connect_tcp(Endpoint{url.host, url.port}, timeout, std::nullopt);
  if (!socket.valid()) {
    result.error = "AI connect failed";
    return result;
  }

  const std::string path = url.path_prefix + "/chat/completions";
  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\n"
      << "Host: " << url.host << "\r\n"
      << "Authorization: Bearer " << api_key << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Connection: close\r\n"
      << "Content-Length: " << body.size() << "\r\n\r\n"
      << body;

  const auto payload = req.str();
  try {
    socket.send_all(payload.data(), payload.size());
  } catch (const std::exception& e) {
    result.error = std::string("AI write failed: ") + e.what();
    return result;
  }

  std::string response_body;
  std::array<std::uint8_t, 4096> buf{};
  socket.set_blocking(false);
  const auto read_deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < read_deadline) {
    const int fd = socket.fd();
    if (fd < 0) break;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(read_deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) break;
    if (net_poll(fd, true, false, static_cast<int>(std::min<std::int64_t>(remaining.count(), 50))) <= 0) continue;
    const auto n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n == 0) break;
    if (n < 0) continue;
    response_body.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(n));
  }

  const auto header_end = response_body.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    result.error = "malformed AI HTTP response";
    return result;
  }
  response_body = response_body.substr(header_end + 4);

  if (response_body.empty()) {
    result.error = "empty HTTP response from AI endpoint";
    return result;
  }

  try {
    auto j = nlohmann::json::parse(response_body);
    if (j.contains("error")) {
      result.error = j["error"].dump();
      return result;
    }
    result.content = j["choices"][0]["message"]["content"].get<std::string>();
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("AI response parse failed: ") + e.what();
  }
  return result;
}

}  // namespace

AiHttpConfig ai_config_from_env() {
  AiHttpConfig cfg;
  if (const char* base = std::getenv("KIKO_AI_BASE_URL")) {
    if (base[0] != '\0') cfg.base_url = base;
  }
  if (const char* model = std::getenv("KIKO_AI_MODEL")) {
    if (model[0] != '\0') cfg.model = model;
  }
  if (const char* key = std::getenv("KIKO_AI_API_KEY")) {
    if (key[0] != '\0') cfg.api_key = key;
  } else if (const char* openai = std::getenv("OPENAI_API_KEY")) {
    if (openai[0] != '\0') cfg.api_key = openai;
  }
  return cfg;
}

bool ai_configured(const AiHttpConfig& config) { return !config.api_key.empty(); }

AiChatResult ai_chat_completion(const AiHttpConfig& config, const std::string& request_json) {
  AiChatResult result;
  if (config.api_key.empty()) {
    result.error = "AI API key not configured";
    return result;
  }
  try {
    const auto url = parse_base_url(config.base_url);
    if (url.https) return curl_https_post(url, config.api_key, request_json, config.timeout);
    return http_post(url, config.api_key, request_json, config.timeout);
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }
}

}  // namespace kiko
