#include "BMSIR_arena_protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace openlr2::arena {

bool IsMd5(const std::string_view value)
{
	if (value.size() != 32) return false;
	return std::ranges::all_of(value, [](const unsigned char item) {
		return std::isxdigit(item) != 0;
	});
}

int NormalizeArenaRandom(const int openLr2Random)
{
	// OpenLR2 0/1/2 are NORMAL/MIRROR/RANDOM. Its value 3 is S-RANDOM,
	// while Arena protocol value 3 means R-RANDOM, so unsupported modifiers
	// must fail safely to NORMAL instead of being mislabeled on the wire.
	return openLr2Random >= 0 && openLr2Random <= 2 ? openLr2Random : 0;
}

int EncodePlayOption(
	const int playMode,
	const int random1P,
	const int random2P,
	const bool flip)
{
	const bool synchronizedRandomSupported =
		playMode == 5 || playMode == 7 || playMode == 10 || playMode == 14;
	const auto normalizeForMode = [synchronizedRandomSupported](const int value) {
		const int normalized = NormalizeArenaRandom(value);
		return normalized == 2 && !synchronizedRandomSupported ? 0 : normalized;
	};
	const int first = normalizeForMode(random1P);
	if (playMode != 10 && playMode != 14 && playMode != 50) return first;
	return first + normalizeForMode(random2P) * 10 + (flip ? 100 : 0);
}

int ArenaClearType(
	const int openLr2ClearType,
	const int openLr2GaugeType,
	const int exscore,
	const int totalNotes,
	const int goodCount)
{
	if (openLr2ClearType <= 1) return 1;
	if (openLr2ClearType == 2) return 4; // EASY CLEAR
	if (openLr2ClearType == 3) return 5; // NORMAL CLEAR
	if (openLr2ClearType == 4) {
		// OpenLR2 stores HARD and DEATH clears under the same lamp.
		return openLr2GaugeType == 2 ? 7 : 6;
	}
	if (openLr2ClearType >= 5) {
		if (totalNotes > 0 && exscore == totalNotes * 2) return 10;
		return goodCount == 0 ? 9 : 8;
	}
	return 1;
}

std::string SceneName(
	const int scene,
	const bool autoplay,
	const bool replay,
	const bool course)
{
	switch (scene) {
		case 2: return "select";
		case 3: return "decide";
		case 4:
			if (course) return "course";
			if (autoplay) return "autoplay";
			if (replay) return "replay";
			return "play";
		case 5: return "result";
		case 13: return "course";
		default: return "unknown";
	}
}

nlohmann::json DiagnosticMessage(
	const std::string_view direction,
	const nlohmann::json& message)
{
	nlohmann::json result = {
		{"event", std::string(direction) + "_message"},
		{"type", message.value("type", "")},
	};
	constexpr std::array stringFields = {
		"match_id", "state", "reason", "code", "queue_status",
		"ruleset_profile", "client_flavor",
	};
	constexpr std::array numberFields = {
		"seq", "player_count", "ready_count", "exscore", "processed_notes",
		"minbp", "max_combo", "play_mode", "play_option",
	};
	for (const auto* field : stringFields) {
		const auto found = message.find(field);
		if (found != message.end() && found->is_string()) {
			result[field] = found->get<std::string>().substr(0, 500);
		}
	}
	for (const auto* field : numberFields) {
		const auto found = message.find(field);
		if (found != message.end() && found->is_number()) result[field] = *found;
	}
	if (const auto chart = message.find("chart");
		chart != message.end() && chart->is_object()) {
		const std::string hash = chart->value("md5", "");
		if (IsMd5(hash)) result["chart_hash"] = hash;
	}
	else {
		const std::string hash = message.value("chart_hash", "");
		if (IsMd5(hash)) result["chart_hash"] = hash;
	}
	if (const auto players = message.find("players");
		players != message.end() && players->is_array()) {
		result["players"] = players->size();
	}
	return result;
}

} // namespace openlr2::arena
