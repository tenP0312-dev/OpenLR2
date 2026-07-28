#pragma once

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace openlr2::arena {

inline constexpr int kProtocolVersion = 5;
inline constexpr std::string_view kClientFlavor = "openlr2";
inline constexpr std::string_view kRulesetProfile = "lr2";
inline constexpr std::string_view kClientVersion = "0.2.0-dev-openlr2";

enum class CountdownBand {
	Normal,
	Yellow,
	Red,
};

bool IsMd5(std::string_view value);
int NormalizeArenaRandom(int openLr2Random);
int ArenaRandomToOpenLr2(int arenaRandom);
int EncodePlayOption(int playMode, int random1P, int random2P, bool flip);
bool ShouldRestartDpRandomSequence(bool arenaActive, int playMode, int player);
std::string NormalizeRoomCode(std::string_view value);
long long CountdownSeconds(double deadline, double serverNow);
CountdownBand CountdownColorBand(long long seconds);
std::string FirstOwnedCpuCandidate(
	const nlohmann::json& candidates,
	const std::function<bool(std::string_view)>& isOwned);
bool IsBoundedArenaManual(const nlohmann::json& value);
int ArenaClearType(
	int openLr2ClearType,
	int openLr2GaugeType,
	int exscore,
	int totalNotes,
	int goodCount);
std::string SceneName(int scene, bool autoplay, bool replay, bool course);
nlohmann::json DiagnosticMessage(std::string_view direction, const nlohmann::json& message);

} // namespace openlr2::arena
