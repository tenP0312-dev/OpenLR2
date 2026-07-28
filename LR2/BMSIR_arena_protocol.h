#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace openlr2::arena {

inline constexpr int kProtocolVersion = 3;
inline constexpr std::string_view kClientFlavor = "openlr2";
inline constexpr std::string_view kRulesetProfile = "lr2";
inline constexpr std::string_view kClientVersion = "0.1.0-dev-openlr2";

bool IsMd5(std::string_view value);
int NormalizeArenaRandom(int openLr2Random);
int EncodePlayOption(int playMode, int random1P, int random2P, bool flip);
int ArenaClearType(
	int openLr2ClearType,
	int openLr2GaugeType,
	int exscore,
	int totalNotes,
	int goodCount);
std::string SceneName(int scene, bool autoplay, bool replay, bool course);
nlohmann::json DiagnosticMessage(std::string_view direction, const nlohmann::json& message);

} // namespace openlr2::arena
