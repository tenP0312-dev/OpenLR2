#include "BMSIR_arena_protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>

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
	// OpenLR2 0/1/2/3 are NORMAL/MIRROR/RANDOM/S-RANDOM. Arena uses 4 for
	// S-RANDOM because 3 is R-RANDOM. Scatter/Converge are not equivalent to
	// Arena R-RANDOM/SPIRAL and therefore fail closed to NORMAL.
	if (openLr2Random >= 0 && openLr2Random <= 2) return openLr2Random;
	return openLr2Random == 3 ? 4 : 0;
}

int ArenaRandomToOpenLr2(const int arenaRandom)
{
	if (arenaRandom >= 0 && arenaRandom <= 2) return arenaRandom;
	return arenaRandom == 4 ? 3 : 0;
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

bool ShouldRestartDpRandomSequence(
	const bool arenaActive,
	const int playMode,
	const int player)
{
	// OpenLR2 normally consumes one DxLib random sequence across both DP
	// sides. Arena's synchronized seed represents one canonical side layout,
	// so restart that sequence before 2P to match independently seeded clients.
	return arenaActive && player == 1 && (playMode == 10 || playMode == 14);
}

std::string NormalizeRoomCode(const std::string_view value)
{
	std::string result;
	result.reserve(std::min<std::size_t>(value.size(), 6));
	for (const unsigned char character : value) {
		if (std::isspace(character)) continue;
		if (!std::isalnum(character) || result.size() >= 6) return {};
		result.push_back(static_cast<char>(std::toupper(character)));
	}
	return result;
}

long long CountdownSeconds(const double deadline, const double serverNow)
{
	if (!std::isfinite(deadline) || !std::isfinite(serverNow)
		|| deadline <= 0.0 || deadline <= serverNow) {
		return 0;
	}
	const double remaining = std::ceil(deadline - serverNow);
	if (remaining >= static_cast<double>(std::numeric_limits<long long>::max())) {
		return std::numeric_limits<long long>::max();
	}
	return static_cast<long long>(remaining);
}

CountdownBand CountdownColorBand(const long long seconds)
{
	if (seconds <= 5) return CountdownBand::Red;
	if (seconds <= 10) return CountdownBand::Yellow;
	return CountdownBand::Normal;
}

std::string FirstOwnedCpuCandidate(
	const nlohmann::json& candidates,
	const std::function<bool(std::string_view)>& isOwned)
{
	if (!candidates.is_array() || !isOwned) return {};
	int bestBand = -1;
	std::string selected;
	for (const auto& candidate : candidates) {
		if (!candidate.is_object()) continue;
		const std::string hash = candidate.value("md5", "");
		const int band = candidate.value("band", -1);
		if (!IsMd5(hash) || band < 1 || !isOwned(hash)) continue;
		if (band > bestBand) {
			bestBand = band;
			selected = hash;
		}
	}
	return selected;
}

bool IsBoundedArenaManual(const nlohmann::json& value)
{
	if (!value.is_object()) return false;
	const auto title = value.find("title");
	const auto version = value.find("version");
	const auto sections = value.find("sections");
	if (title == value.end() || !title->is_string()
		|| title->get_ref<const std::string&>().size() > 200
		|| version == value.end() || !version->is_string()
		|| version->get_ref<const std::string&>().size() > 80
		|| sections == value.end() || !sections->is_array()
		|| sections->size() > 32) {
		return false;
	}
	for (const auto& section : *sections) {
		if (!section.is_object()) return false;
		const auto sectionTitle = section.find("title");
		const auto items = section.find("items");
		if (sectionTitle == section.end() || !sectionTitle->is_string()
			|| sectionTitle->get_ref<const std::string&>().size() > 200
			|| items == section.end() || !items->is_array()
			|| items->size() > 32) {
			return false;
		}
		for (const auto& item : *items) {
			if (!item.is_string() || item.get_ref<const std::string&>().size() > 1000) {
				return false;
			}
		}
	}
	return true;
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
		"ruleset_profile", "client_flavor", "room_code", "room_name",
	};
	constexpr std::array numberFields = {
		"seq", "player_count", "ready_count", "exscore", "processed_notes",
		"minbp", "max_combo", "play_mode", "play_option", "target_band",
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
