#pragma once

#include "ai_client.hpp"
#include "connectivity.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace kiko {

struct AiAdvisorResult {
  bool ok = false;
  std::string text;
  std::optional<RoutePlan> plan;
  std::optional<nlohmann::json> parsed_plan;
  std::string error;
};

[[nodiscard]] std::string connectivity_snapshot_to_json(const ConnectivitySnapshot& snapshot);
[[nodiscard]] RoutePlan validate_ai_route_plan(const nlohmann::json& j);
[[nodiscard]] RoutePlan merge_ai_route_plan(const RoutePlan& rules, const RoutePlan& ai_plan,
                                           const nlohmann::json* raw_keys = nullptr);

// Rule-based retry hints after a failed direct attempt (no LLM).
[[nodiscard]] std::string rule_direct_failure_hints(const ConnectivitySnapshot& snapshot, const RoutePlan& plan);

[[nodiscard]] AiAdvisorResult ai_explain_diagnosis(const ConnectivitySnapshot& snapshot, const RoutePlan& rules,
                                                   const std::string& rule_diagnosis, const AiHttpConfig& config);
[[nodiscard]] AiAdvisorResult ai_suggest_route_plan(const ConnectivitySnapshot& snapshot, const AiHttpConfig& config);

}  // namespace kiko
