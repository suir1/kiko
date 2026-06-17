#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace kiko {

struct AiHttpConfig {
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string model = "gpt-4o-mini";
  std::chrono::milliseconds timeout{800};
};

struct AiChatResult {
  bool ok = false;
  std::string content;
  std::string error;
};

[[nodiscard]] AiHttpConfig ai_config_from_env();
[[nodiscard]] bool ai_configured(const AiHttpConfig& config);

// OpenAI-compatible chat completions POST. Sends only the provided JSON body.
[[nodiscard]] AiChatResult ai_chat_completion(const AiHttpConfig& config, const std::string& request_json);

}  // namespace kiko
