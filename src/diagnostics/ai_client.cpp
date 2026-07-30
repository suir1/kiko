#include "ai_client.hpp"

#include "core/common.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

namespace kiko {
namespace {

constexpr std::size_t kMaxAiResponseBytes = 2 * 1024 * 1024;

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

AiChatResult parse_ai_response(const std::string& response) {
  try {
    const auto body = nlohmann::json::parse(response);
    if (body.contains("error")) return {.error = body["error"].dump()};
    return {.ok = true, .content = body["choices"][0]["message"]["content"].get<std::string>()};
  } catch (const std::exception& e) {
    return {.error = std::string("AI response parse failed: ") + e.what()};
  }
}

struct CurlWriteState {
  std::string body;
  bool exceeded_limit = false;
};

std::size_t append_curl_response(char* data, std::size_t size, std::size_t count, void* user_data) {
  auto& state = *static_cast<CurlWriteState*>(user_data);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    state.exceeded_limit = true;
    return 0;
  }
  const auto bytes = size * count;
  if (bytes > kMaxAiResponseBytes - std::min(state.body.size(), kMaxAiResponseBytes)) {
    state.exceeded_limit = true;
    return 0;
  }
  state.body.append(data, bytes);
  return bytes;
}

std::optional<std::string> append_header(curl_slist*& headers, const std::string& value) {
  auto* updated = curl_slist_append(headers, value.c_str());
  if (updated == nullptr) return "failed to allocate AI request headers";
  headers = updated;
  return std::nullopt;
}

AiChatResult curl_post(const ParsedUrl& url, const std::string& api_key, const std::string& body,
                       std::chrono::milliseconds timeout) {
  AiChatResult result;
  if (api_key.find_first_of("\r\n") != std::string::npos) {
    result.error = "AI API key contains invalid line break";
    return result;
  }
  if (body.size() > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max())) {
    result.error = "AI request body is too large";
    return result;
  }

  static const CURLcode global_init = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (global_init != CURLE_OK) {
    result.error = std::string("failed to initialize libcurl: ") + curl_easy_strerror(global_init);
    return result;
  }

  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
  if (!handle) {
    result.error = "failed to initialize AI HTTP request";
    return result;
  }

  const auto host = url.host.find(':') == std::string::npos ? url.host : "[" + url.host + "]";
  const std::string target = std::string(url.https ? "https://" : "http://") + host + ":" +
                             std::to_string(url.port) + url.path_prefix + "/chat/completions";
  curl_slist* raw_headers = nullptr;
  if (auto error = append_header(raw_headers, "Authorization: Bearer " + api_key)) return {.error = *error};
  if (auto error = append_header(raw_headers, "Content-Type: application/json")) {
    curl_slist_free_all(raw_headers);
    return {.error = *error};
  }
  const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);

  const auto timeout_ms = std::clamp<std::int64_t>(timeout.count(), 1, std::numeric_limits<long>::max());
  CurlWriteState write_state;
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  const auto set_options =
      curl_easy_setopt(handle.get(), CURLOPT_URL, target.c_str()) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, raw_headers) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_POST, 1L) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.data()) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms)) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms)) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxAiResponseBytes)) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_curl_response) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &write_state) == CURLE_OK &&
      curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data()) == CURLE_OK;
  if (!set_options || (is_loopback_host(url.host) && curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*") != CURLE_OK)) {
    result.error = "failed to configure AI HTTP request";
    return result;
  }

  const auto request_status = curl_easy_perform(handle.get());
  long http_status = 0;
  (void)curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_status);

  if (write_state.exceeded_limit || request_status == CURLE_FILESIZE_EXCEEDED) {
    result.error = "AI response exceeds " + std::to_string(kMaxAiResponseBytes) + " byte limit";
    return result;
  }
  if (request_status != CURLE_OK) {
    result.error = "AI request failed";
    const auto detail = error_buffer.front() == '\0' ? curl_easy_strerror(request_status) : error_buffer.data();
    if (detail[0] != '\0') result.error += ": " + std::string(detail);
    return result;
  }

  if (http_status < 200 || http_status >= 300) {
    result.error = "AI HTTP " + std::to_string(http_status);
    if (!trim(write_state.body).empty()) {
      const auto parsed_error = parse_ai_response(write_state.body);
      result.error += ": " + (parsed_error.error.empty() ? trim(write_state.body) : parsed_error.error);
    }
    return result;
  }
  if (write_state.body.empty()) {
    result.error = "empty response from AI endpoint";
    return result;
  }
  return parse_ai_response(write_state.body);
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
    return curl_post(url, config.api_key, request_json, config.timeout);
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }
}

}  // namespace kiko
