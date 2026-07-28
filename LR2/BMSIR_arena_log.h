#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace openlr2::arena {

void LogEvent(std::string_view event);
void LogEvent(std::string_view event, nlohmann::json details);
void LogMessage(std::string_view direction, const nlohmann::json& message);

} // namespace openlr2::arena
