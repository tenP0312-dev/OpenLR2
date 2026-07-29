#include "BMSIR_arena_protocol.h"

#include <nlohmann/json.hpp>

#include <cassert>

using namespace openlr2::arena;

int main()
{
	assert(kProtocolVersion == 6);
	assert(kClientVersion == "0.4.0-dev-openlr2");
	assert(IsMd5("0123456789abcdef0123456789ABCDEF"));
	assert(!IsMd5("0123"));
	assert(!IsMd5("z123456789abcdef0123456789abcdef"));
	assert(NormalizeArenaRandom(0) == 0);
	assert(NormalizeArenaRandom(2) == 2);
	assert(NormalizeArenaRandom(3) == 4);
	assert(NormalizeArenaRandom(4) == 0);
	assert(ArenaRandomToOpenLr2(0) == 0);
	assert(ArenaRandomToOpenLr2(4) == 3);
	assert(ArenaRandomToOpenLr2(3) == 0);
	assert(EncodePlayOption(7, 2, 1, true) == 2);
	assert(EncodePlayOption(14, 2, 1, true) == 112);
	assert(EncodePlayOption(9, 2, 0, false) == 0);
	assert(EncodePlayOption(50, 2, 2, true) == 100);
	assert(EncodePlayOption(14, 3, 3, false) == 44);
	assert(NormalizeRoomCode(" ab 12cd ") == "AB12CD");
	assert(NormalizeRoomCode("AB-CD").empty());
	assert(CountdownSeconds(110.1, 100.0) == 11);
	assert(CountdownSeconds(100.0, 100.0) == 0);
	assert(CountdownColorBand(11) == CountdownBand::Normal);
	assert(CountdownColorBand(10) == CountdownBand::Yellow);
	assert(CountdownColorBand(5) == CountdownBand::Red);
	assert(ShouldRestartDpRandomSequence(true, 10, 1));
	assert(ShouldRestartDpRandomSequence(true, 14, 1));
	assert(!ShouldRestartDpRandomSequence(true, 14, 0));
	assert(!ShouldRestartDpRandomSequence(true, 7, 1));
	assert(!ShouldRestartDpRandomSequence(false, 14, 1));
	assert(ArenaClearType(1, 0, 0, 100, 0) == 1);
	assert(ArenaClearType(2, 3, 100, 100, 0) == 4);
	assert(ArenaClearType(3, 0, 150, 100, 0) == 5);
	assert(ArenaClearType(4, 1, 150, 100, 0) == 6);
	assert(ArenaClearType(4, 2, 150, 100, 0) == 7);
	assert(ArenaClearType(5, 4, 180, 100, 1) == 8);
	assert(ArenaClearType(5, 4, 190, 100, 0) == 9);
	assert(ArenaClearType(5, 4, 200, 100, 0) == 10);
	assert(SceneName(2, false, false, false) == "select");
	assert(SceneName(4, false, false, false) == "play");
	assert(SceneName(4, true, false, false) == "autoplay");

	const nlohmann::json cpuCandidates = nlohmann::json::array({
		{{"md5", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, {"band", 12}},
		{{"md5", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}, {"band", 10}},
	});
	assert(FirstOwnedCpuCandidate(
		cpuCandidates,
		[](const std::string_view hash) { return hash.starts_with("b"); })
		== "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	assert(FirstOwnedCpuCandidate(
		cpuCandidates,
		[](const std::string_view) { return false; }).empty());
	assert(IsBoundedArenaManual({
		{"version", "1"},
		{"title", "Arena"},
		{"sections", nlohmann::json::array({
			{{"title", "Start"}, {"items", nlohmann::json::array({"One"})}},
		})},
	}));
	assert(!IsBoundedArenaManual({
		{"version", "1"},
		{"title", "Arena"},
		{"sections", "not-an-array"},
	}));

	const nlohmann::json message = {
		{"type", "hello"},
		{"passmd5", "must-not-appear"},
		{"build_hash", "must-not-appear"},
		{"text", "must-not-appear"},
		{"match_id", "match-1"},
		{"chart", {{"md5", "0123456789abcdef0123456789abcdef"}}},
	};
	const auto diagnostic = DiagnosticMessage("outbound", message);
	assert(diagnostic.value("type", "") == "hello");
	assert(diagnostic.value("match_id", "") == "match-1");
	assert(diagnostic.value("chart_hash", "")
		== "0123456789abcdef0123456789abcdef");
	assert(!diagnostic.contains("passmd5"));
	assert(!diagnostic.contains("build_hash"));
	assert(!diagnostic.contains("text"));
	return 0;
}
