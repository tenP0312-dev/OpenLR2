#include "BMSIR_arena.h"

#include "BMSIR_arena_log.h"
#include "BMSIR_arena_protocol.h"
#include "BMSIR_arena_transport.h"
#include "En_fileutil.h"
#include "LR2.h"
#include "LR2_songmanage.h"
#include "LR2_version.h"
#include "Scene02_Songselect.h"
#include "structure.h"

#include <DxLib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#ifndef OPENLR2_ARENA_BUILD_HASH
#define OPENLR2_ARENA_BUILD_HASH "unknown"
#endif

namespace openlr2::arena {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPanelToggleKey = KEY_INPUT_F9;
constexpr int kOverlayToggleKey = KEY_INPUT_F10;
constexpr int kForceEndKey = KEY_INPUT_END;
constexpr int kPanelPageCount = 5;
constexpr int kMaxChatMessages = 20;

struct RoomSettings {
	std::string roomName{"OpenLR2 Arena room"};
	std::string scoreRule{"exscore"};
	std::string forcedGauge{"free"};
	std::string chartScope{"official"};
	std::string nominationPolicy{"all"};
	int nominationSeconds{60};
	int optionSeconds{10};
	int intermissionSeconds{};
	std::string seriesFormat{"single"};
	int firstToWins{2};
	bool spectatorPublic{true};
	bool forceHostOption{};
};

struct ArenaConfig {
	bool enabled{};
	bool showOverlay{true};
	bool allowCpu{true};
	bool unrestrictedRating{};
	bool stayInRoom{true};
	int playerId{};
	std::string server{"wss://www.bms-ir.org/new/arena/ws/client"};
	RoomSettings room;
};

enum class TextTarget {
	None,
	JoinRoomCode,
	JoinRoomPassword,
	RoomName,
	RoomPassword,
	RoomChat,
	LobbyChat,
};

std::string DisplayString(const std::string_view utf8)
{
	return utf2ansi(utf8, 932);
}

template <typename T>
void CycleValue(T& value, const std::vector<T>& choices, const int direction)
{
	if (choices.empty()) return;
	auto found = std::find(choices.begin(), choices.end(), value);
	std::size_t index = found == choices.end()
		? 0
		: static_cast<std::size_t>(found - choices.begin());
	if (direction < 0) {
		index = index == 0 ? choices.size() - 1 : index - 1;
	}
	else {
		index = (index + 1) % choices.size();
	}
	value = choices[index];
}

class Client {
public:
	explicit Client(game* gameState)
		: game_(gameState),
		  config_(ReadConfig()),
		  overlayVisible_(config_.showOverlay)
	{
		LogEvent("initialize", {
			{"enabled", config_.enabled},
			{"client_version", std::string(kClientVersion)},
			{"body_version", openlr2::versionName},
			{"client_flavor", std::string(kClientFlavor)},
			{"protocol", kProtocolVersion},
		});
		if (!config_.enabled) {
			status_ = "disabled (create LR2files/Config/bmsir-arena.json)";
			return;
		}
		if (config_.playerId <= 0) {
			config_.playerId = game_->net.rankingData.myID > 0
				? game_->net.rankingData.myID
				: game_->net.IR_ID;
		}
		if (config_.playerId <= 0 || game_->config.player.passMD5.length() != 32) {
			status_ = "configuration error; see bmsir-arena.log";
			LogEvent("configuration_invalid", {
				{"has_player_id", config_.playerId > 0},
				{"has_passmd5", game_->config.player.passMD5.length() == 32},
			});
			return;
		}
		status_ = "connecting";
		lastConnectAttempt_ = Clock::now();
		transport_.Start(config_.server);
		LogEvent("connect_requested", {{"player_id", config_.playerId}});
	}

	~Client()
	{
		Shutdown();
	}

	void Shutdown()
	{
		if (shutdown_) return;
		shutdown_ = true;
		CloseTextInput();
		if (reserved_ && !matchId_.empty() && !finalSent_) {
			Send(MatchMessage("forfeit", {{"reason", "client_shutdown"}}));
		}
		transport_.Stop();
		RestoreOptions();
		LogEvent("shutdown");
	}

	void Tick(game* gameState, sqlite3* database)
	{
		game_ = gameState;
		database_ = database;
		if (!config_.enabled || shutdown_) return;
		HandleTransportState();
		for (auto& raw : transport_.DrainMessages()) {
			try {
				auto message = nlohmann::json::parse(raw);
				if (!message.is_object()) throw std::runtime_error("object required");
				LogMessage("inbound", message);
				HandleMessage(message);
			}
			catch (const std::exception& error) {
				LogEvent("message_parse_failed", {{"error", error.what()}});
			}
		}
		const auto now = Clock::now();
		if (authenticated_ && now - lastPing_ >= std::chrono::seconds(5)) {
			const double clientTime = EpochSeconds();
			Send({{"type", "ping"}, {"client_time", clientTime}});
			lastPing_ = now;
		}
		UpdateScene();
		PollTextInput();
		HandleUiKeys();
		HandleArenaHotkeys();
		SendLiveIfDue();
		if (!transport_.Running()
			&& now - lastConnectAttempt_ >= std::chrono::seconds(5)) {
			helloSent_ = false;
			authenticated_ = false;
			lastConnectAttempt_ = now;
			status_ = "reconnecting";
			transport_.Start(config_.server);
			LogEvent("reconnect_requested");
		}
	}

	void Draw(const game*) const
	{
		if (!config_.enabled || !overlayVisible_) return;
		const int connectedColor = authenticated_
			? GetColor(180, 255, 180)
			: GetColor(255, 210, 120);
		const int background = GetColor(12, 12, 18);
		SetDrawBlendMode(DX_BLENDMODE_ALPHA, 220);
		DrawBox(8, 8, 862, 116, background, TRUE);
		SetDrawBlendMode(DX_BLENDMODE_NOBLEND, 255);
		DrawString(16, 14, "BMS-IR Arena for OpenLR2 0.2.0 / protocol v5", connectedColor);
		DrawFormatString(
			16,
			34,
			GetColor(235, 235, 240),
			"F9: control panel  F10: overlay  Insert: primary action  status: %s",
			status_.c_str());
		const auto [action, seconds, deadline] = PhaseActionAndCountdown();
		const int actionColor = deadline
			? CountdownColor(seconds)
			: GetColor(190, 220, 255);
			DrawExtendString(
				16,
				52,
				1.2,
				1.2,
				action.c_str(),
				actionColor);
		if (deadline) {
			DrawFormatString(650, 52, actionColor, "%lld sec", seconds);
		}
		DrawFormatString(
			16,
			78,
			GetColor(180, 180, 195),
			"rating: %.0f / matches: %d / queue: %s / room: %s",
			arenaRating_,
			arenaMatchesPlayed_,
			queueStatus_.c_str(),
			roomCode_.empty() ? "-" : roomCode_.c_str());
		DrawFormatString(
			16,
			98,
			GetColor(160, 160, 175),
			"phase: %s / match: %.12s / diagnostics: bmsir-arena.log",
			phase_.c_str(),
			matchId_.c_str());

		int nextY = 124;
		if (liveView_.is_object() && liveView_.contains("players")) {
			nextY = DrawBattle(nextY);
		}
		if (resultVisible_ && resultView_.is_object()) {
			nextY = DrawResult(nextY);
		}
		if (panelOpen_) {
			DrawPanel(std::max(nextY + 6, 126));
		}
	}

	bool ConsumePreparedChart(game* gameState, sqlite3* database)
	{
		if (!launchRequested_ || !active_ || gameState->procSelecter != 2) return false;
		SONGDATA song{};
		InitSongData(&song);
		if (!IsMd5(chartHash_)
			|| GetSongData(CSTR(chartHash_.c_str()), &song, database, &gameState->sSelect) != 1) {
			LogEvent("prepared_chart_missing", {
				{"match_id", matchId_},
				{"chart_hash", chartHash_},
			});
			Send(MatchMessage("forfeit", {{"reason", "prepared_chart_missing"}}));
			launchRequested_ = false;
			return false;
		}
		COPY_SONGDATA(
			&gameState->sSelect.bmsList[gameState->sSelect.cur_song],
			&song);
		gameState->gameplay.courseType = -1;
		gameState->gameplay.isCourse = 0;
		gameState->gameplay.courseStageCount = 0;
		gameState->gameplay.isAutoplay = 0;
		gameState->gameplay.replay.status = 0;
		gameState->net.rankingData.target_ID = 0;
		ProcS_Select(gameState);
		ApplyPlaySettings(gameState);
		gameState->procSelecter = 3;
		launchRequested_ = false;
		LogEvent("chart_launch_requested", {
			{"match_id", matchId_},
			{"chart_hash", chartHash_},
			{"play_mode", playMode_},
		});
		return true;
	}

	void ApplyPlaySettings(game* gameState)
	{
		if (!active_) return;
		if (!savedPlayConfig_) savedPlayConfig_ = gameState->config.play;
		auto& play = gameState->config.play;
		play.autokey = false;
		play.autojudge = 0;
		play.assist[PLAYER_1] = 0;
		play.assist[PLAYER_2] = 0;
		const bool doublePlay = IsDoublePlay(playMode_);
		play.random[PLAYER_1] = ArenaRandomToOpenLr2(playOption_ % 10);
		play.random[PLAYER_2] = doublePlay
			? ArenaRandomToOpenLr2((playOption_ / 10) % 10)
			: OPTION_RANDOM_OFF;
		play.dpFlip = doublePlay && (playOption_ / 100) % 10 == 1;
		play.randSC[PLAYER_1] = play.randSC[PLAYER_2] = 0;
		play.randFix[PLAYER_1] = play.randFix[PLAYER_2] = 0;
		play.m_addmine = 0;
		play.m_addlong = 0;
		play.m_addnote = 0;
		play.m_isExtra = false;
		play.m_extra = 0;
		play.battle = OPTION_BATTLE_OFF;
		if (forcedGauge_ == "normal") {
			play.gaugeType[PLAYER_1] = play.gaugeType[PLAYER_2] = OPTION_GAUGE_GROOVE;
		}
		else if (forcedGauge_ == "hard") {
			play.gaugeType[PLAYER_1] = play.gaugeType[PLAYER_2] = OPTION_GAUGE_HARD;
		}
		else if (forcedGauge_ == "exhard") {
			play.gaugeType[PLAYER_1] = play.gaugeType[PLAYER_2] = OPTION_GAUGE_DEATH;
		}
		else if (forcedGauge_ == "hazard") {
			play.gaugeType[PLAYER_1] = play.gaugeType[PLAYER_2] = OPTION_GAUGE_PATTACK;
		}
		gameState->gameplay.randomseed = static_cast<int>(randomSeed_);
		gameState->net.rankingData.target_ID = 0;
		LogEvent("play_options_applied", {
			{"match_id", matchId_},
			{"play_mode", playMode_},
			{"play_option", playOption_},
			{"gauge", play.gaugeType[PLAYER_1]},
			{"random_seed", randomSeed_},
		});
	}

	bool WaitForStart(game* gameState)
	{
		if (!active_) return true;
		const std::string matchId = matchId_;
		const std::string chartHash = chartHash_;
		if (!playReadySent_.exchange(true)) {
			Send({
				{"type", "play_ready"},
				{"match_id", matchId},
				{"chart_hash", chartHash},
			});
			LogEvent("play_ready", {
				{"match_id", matchId},
				{"chart_hash", chartHash},
			});
		}
		while (active_ && !shutdown_) {
			if (gameState->gameplay.flag_closingPhase) return false;
			const double start = startAt_.load();
			if (start > 0.0 && ServerNow() >= start) {
				LogEvent("synchronized_start_released", {
					{"match_id", matchId},
					{"chart_hash", chartHash},
				});
				return true;
			}
			const double deadline = loadDeadline_.load();
			if (deadline > 0.0 && ServerNow() > deadline + 5.0) {
				Send({
					{"type", "forfeit"},
					{"match_id", matchId},
					{"reason", "openlr2_load_deadline_exceeded"},
				});
				LogEvent("synchronized_start_timeout", {
					{"match_id", matchId},
					{"chart_hash", chartHash},
				});
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return false;
	}

	[[nodiscard]] bool Active() const { return active_; }
	[[nodiscard]] bool ArenaChart() const { return arenaChart_; }

private:
	static ArenaConfig ReadConfig()
	{
		ArenaConfig result;
		const std::filesystem::path path = "LR2files/Config/bmsir-arena.json";
		try {
			if (!std::filesystem::exists(path)) return result;
			std::ifstream stream(path, std::ios::binary);
			const auto value = nlohmann::json::parse(stream);
			result.enabled = value.value("enabled", false);
			result.showOverlay = value.value("show_overlay", true);
			result.allowCpu = value.value("allow_cpu", true);
			result.unrestrictedRating = value.value("unrestricted_rating", false);
			result.stayInRoom = value.value("stay_in_room", true);
			result.playerId = value.value("player_id", 0);
			result.server = value.value(
				"server",
				std::string("wss://www.bms-ir.org/new/arena/ws/client"));
			const auto room = value.value("room_defaults", nlohmann::json::object());
			result.room.roomName = room.value("room_name", result.room.roomName);
			result.room.scoreRule = room.value("score_rule", result.room.scoreRule);
			result.room.forcedGauge = room.value("forced_gauge", result.room.forcedGauge);
			result.room.chartScope = room.value("chart_scope", result.room.chartScope);
			result.room.nominationPolicy = room.value(
				"nomination_policy",
				result.room.nominationPolicy);
			result.room.nominationSeconds = std::clamp(
				room.value("nomination_seconds", result.room.nominationSeconds),
				10,
				180);
			result.room.optionSeconds = std::clamp(
				room.value("option_seconds", result.room.optionSeconds),
				5,
				60);
			result.room.intermissionSeconds = std::clamp(
				room.value("intermission_seconds", result.room.intermissionSeconds),
				0,
				60);
			result.room.seriesFormat = room.value(
				"series_format",
				result.room.seriesFormat);
			result.room.firstToWins = std::clamp(
				room.value("first_to_wins", result.room.firstToWins),
				2,
				5);
			result.room.spectatorPublic = room.value("spectator_public", true);
			result.room.forceHostOption = room.value("force_host_option", false);
			if (!result.server.starts_with("wss://")
				&& !result.server.starts_with("ws://")) {
				throw std::runtime_error("server must use ws:// or wss://");
			}
		}
		catch (const std::exception& error) {
			LogEvent("configuration_parse_failed", {{"error", error.what()}});
			result.enabled = false;
		}
		return result;
	}

	void WriteConfig() const
	{
		const std::filesystem::path path = "LR2files/Config/bmsir-arena.json";
		const std::filesystem::path temporary = path.string() + ".tmp";
		try {
			std::filesystem::create_directories(path.parent_path());
			const nlohmann::json value = {
				{"enabled", config_.enabled},
				{"server", config_.server},
				{"player_id", config_.playerId},
				{"show_overlay", config_.showOverlay},
				{"allow_cpu", config_.allowCpu},
				{"unrestricted_rating", config_.unrestrictedRating},
				{"stay_in_room", config_.stayInRoom},
				{"room_defaults", {
					{"room_name", config_.room.roomName},
					{"score_rule", config_.room.scoreRule},
					{"forced_gauge", config_.room.forcedGauge},
					{"chart_scope", config_.room.chartScope},
					{"nomination_policy", config_.room.nominationPolicy},
					{"nomination_seconds", config_.room.nominationSeconds},
					{"option_seconds", config_.room.optionSeconds},
					{"intermission_seconds", config_.room.intermissionSeconds},
					{"series_format", config_.room.seriesFormat},
					{"first_to_wins", config_.room.firstToWins},
					{"spectator_public", config_.room.spectatorPublic},
					{"force_host_option", config_.room.forceHostOption},
				}},
			};
			{
				std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
				stream << value.dump(2) << '\n';
				if (!stream) throw std::runtime_error("config write failed");
			}
			std::error_code error;
			std::filesystem::remove(path, error);
			std::filesystem::rename(temporary, path);
		}
		catch (const std::exception& error) {
			LogEvent("configuration_write_failed", {{"error", error.what()}});
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
		}
	}

	static double EpochSeconds()
	{
		return std::chrono::duration<double>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	[[nodiscard]] double ServerNow() const
	{
		return EpochSeconds() + serverOffset_.load();
	}

	static bool IsDoublePlay(const int playMode)
	{
		return playMode == 10 || playMode == 14 || playMode == 50;
	}

	void HandleTransportState()
	{
		const bool connected = transport_.Connected();
		if (connected && !transportWasConnected_) {
			transportWasConnected_ = true;
			status_ = "socket connected; authenticating";
			LogEvent("socket_open");
		}
		else if (!connected && transportWasConnected_) {
			transportWasConnected_ = false;
			authenticated_ = false;
			helloSent_ = false;
			status_ = "disconnected";
			LogEvent("socket_closed", {
				{"error", transport_.LastError()},
				{"match_id", matchId_},
			});
		}
		if (connected && !helloSent_) {
			nlohmann::json hello = {
			{"type", "hello"},
				{"protocol", kProtocolVersion},
				{"player_id", config_.playerId},
				{"passmd5", game_->config.player.passMD5.body},
				{"client_version", std::string(kClientVersion)},
				{"body_version", openlr2::versionName},
				{"build_hash", OPENLR2_ARENA_BUILD_HASH},
				{"arena_enabled", true},
				{"client_flavor", std::string(kClientFlavor)},
				{"ruleset_profile", std::string(kRulesetProfile)},
				{"server_cpu_v1", true},
				{"server_cpu_catalog_v1", true},
			};
			helloSent_ = transport_.Send(hello.dump());
			if (helloSent_) {
				LogEvent("hello_sent", {
					{"player_id", config_.playerId},
					{"client_version", std::string(kClientVersion)},
					{"client_flavor", std::string(kClientFlavor)},
					{"protocol", kProtocolVersion},
				});
			}
		}
		if (!transport_.Running() && !transport_.LastError().empty()
			&& transport_.LastError() != lastTransportError_) {
			lastTransportError_ = transport_.LastError();
			status_ = lastTransportError_;
			LogEvent("transport_error", {{"error", lastTransportError_}});
		}
	}

	bool IsCurrentMatchMessage(const nlohmann::json& message) const
	{
		return !matchId_.empty() && message.value("match_id", "") == matchId_;
	}

	void HandleMessage(const nlohmann::json& message)
	{
		const std::string type = message.value("type", "");
		if (type == "hello_ok") {
			if (message.value("ruleset_profile", "") != kRulesetProfile
				|| message.value("protocol", 0) != kProtocolVersion) {
				status_ = "server capability mismatch";
				LogEvent("hello_capability_mismatch", {
					{"ruleset_profile", message.value("ruleset_profile", "")},
					{"protocol", message.value("protocol", 0)},
				});
				transport_.Stop();
				return;
			}
			authenticated_ = true;
			status_ = "connected; F9 opens Arena controls";
			UpdateClock(message);
			SendState();
			RequestStatus();
			RequestManual();
			return;
		}
		if (type == "pong") {
			UpdateClock(message);
			return;
		}
		if (type == "arena_status") {
			ReceiveArenaStatus(message);
			return;
		}
		if (type == "arena_manual") {
			if (IsBoundedArenaManual(message)) {
				manualView_ = message;
				manualSection_ = 0;
				LogEvent("arena_manual_received", {
					{"version", message.value("version", "")},
					{"sections", message["sections"].size()},
				});
			}
			else {
				LogEvent("arena_manual_rejected");
			}
			return;
		}
		if (type == "room_status") {
			ReceiveRoomStatus(message);
			return;
		}
		if (type == "lobby_chat_history") {
			lobbyChat_ = message.value("messages", nlohmann::json::array());
			TrimChat(lobbyChat_);
			return;
		}
		if (type == "lobby_chat") {
			AppendChat(lobbyChat_, message);
			return;
		}
		if (type == "chat_history") {
			if (message.value("room_code", "").empty()
				&& !IsCurrentMatchMessage(message)) return;
			matchChat_ = message.value("messages", nlohmann::json::array());
			TrimChat(matchChat_);
			return;
		}
		if (type == "chat") {
			const std::string incomingRoom = message.value("room_code", "");
			if ((!incomingRoom.empty() && incomingRoom == roomCode_)
				|| IsCurrentMatchMessage(message)) {
				AppendChat(matchChat_, message);
			}
			return;
		}
		if (type == "fill_started") {
			if (!IsCurrentMatchMessage(message)) return;
			phase_ = "filling";
			fillDeadline_ = message.value("deadline", 0.0);
			status_ = "waiting for room participants";
			return;
		}
		if (type == "players_updated") return;
		if (type == "match_reserved") {
			ReceiveMatchReserved(message);
			return;
		}
		if (type == "nomination_started" || type == "nomination_status") {
			if (!IsCurrentMatchMessage(message)) return;
			ReceiveNominationStatus(message);
			return;
		}
		if (type == "cpu_chart_request") {
			if (!IsCurrentMatchMessage(message)) return;
			RespondToCpuChartRequest(message);
			return;
		}
		if (type == "nomination_accepted") {
			if (!IsCurrentMatchMessage(message)) return;
			status_ = "nomination accepted";
			return;
		}
		if (type == "chart_candidate_rejected") {
			if (!IsCurrentMatchMessage(message)) return;
			status_ = "chart candidate unavailable; trying another";
			return;
		}
		if (type == "nominations_revealed") {
			if (!IsCurrentMatchMessage(message)) return;
			phase_ = "chart check";
			return;
		}
		if (type == "chart") {
			if (!IsCurrentMatchMessage(message)) return;
			ReceiveChart(message);
			return;
		}
		if (type == "option_select") {
			if (!IsCurrentMatchMessage(message)) return;
			ReceiveRules(message);
			phase_ = "options";
			playModeLabel_ = message.value("play_mode_label", playModeLabel_);
			optionDeadline_ = message.value("deadline", 0.0);
			optionReadySent_ = message.value("ready", false);
			status_ = optionReadySent_
				? "option locked; waiting for others"
				: "choose lane option; Insert to READY";
			return;
		}
		if (type == "prepare") {
			if (!IsCurrentMatchMessage(message)) return;
			ReceiveRules(message);
			phase_ = "loading";
			randomSeed_ = message.value("random_seed", randomSeed_);
			playOption_ = message.value("play_option", playOption_);
			playMode_ = message.value("play_mode", playMode_);
			playModeLabel_ = message.value("play_mode_label", playModeLabel_);
			loadDeadline_ = message.value("load_deadline", 0.0);
			active_ = true;
			arenaChart_ = true;
			launchRequested_ = true;
			status_ = "loading Arena chart";
			LogEvent("prepare_received", {
				{"match_id", matchId_},
				{"chart_hash", chartHash_},
				{"play_mode", playMode_},
				{"play_option", playOption_},
				{"random_seed", randomSeed_},
				{"load_deadline", loadDeadline_.load()},
			});
			return;
		}
		if (type == "start") {
			if (!IsCurrentMatchMessage(message)) return;
			startAt_ = message.value("start_at", 0.0);
			phase_ = "countdown";
			status_ = "shared start scheduled";
			LogEvent("start_received", {
				{"match_id", matchId_},
				{"start_at", startAt_.load()},
				{"server_offset_seconds", serverOffset_.load()},
			});
			return;
		}
		if (type == "live") {
			if (!IsCurrentMatchMessage(message)) return;
			liveView_ = message;
			const std::string remoteState = message.value("state", "");
			if (!remoteState.empty()) phase_ = remoteState;
			if (const auto deadline = message.find("fill_deadline");
				deadline != message.end() && deadline->is_number()) {
				fillDeadline_ = deadline->get<double>();
			}
			if (const auto option = message.find("option_selection");
				option != message.end() && option->is_object()) {
				if (const auto deadline = option->find("deadline");
					deadline != option->end() && deadline->is_number()) {
					optionDeadline_ = deadline->get<double>();
				}
			}
			if (const auto nomination = message.find("nomination");
				nomination != message.end() && nomination->is_object()) {
				if (const auto deadline = nomination->find("deadline");
					deadline != nomination->end() && deadline->is_number()) {
					nominationDeadline_ = deadline->get<double>();
				}
			}
			return;
		}
		if (type == "result") {
			if (!IsCurrentMatchMessage(message)) return;
			ReceiveResult(message);
			return;
		}
		if (type == "force_end_approved") {
			if (!IsCurrentMatchMessage(message)) return;
			forceEndVoteSent_ = true;
			finalSent_ = true;
			active_ = false;
			status_ = "force end approved";
			SetTimeLapse(2, &game_->timer1);
			game_->procPhase = 2;
			game_->gameplay.flag_closingPhase = 1;
			return;
		}
		if (type == "match_cancelled" || type == "match_released"
			|| type == "forfeit_accepted") {
			if (!message.value("match_id", "").empty()
				&& !IsCurrentMatchMessage(message)) return;
			status_ = message.value("reason", type);
			const bool retained = message.value("queue_retained", false);
			queueStatus_ = retained ? "queued" : "cancelled";
			EndMatch(false);
			RequestStatus();
			return;
		}
		if (type == "room_disbanded" || type == "room_kicked"
			|| type == "room_closed") {
			status_ = type;
			roomCode_.clear();
			roomView_ = nlohmann::json::object();
			matchChat_ = nlohmann::json::array();
			queueStatus_ = "cancelled";
			roomReady_ = false;
			EndMatch(false);
			RequestStatus();
			return;
		}
		if (type == "match_resume") {
			if (!IsCurrentMatchMessage(message)) return;
			const std::string state = message.value("state", "");
			if (active_ && state == "countdown") {
				startAt_ = message.value("start_at", 0.0);
			}
			else if (!active_ && (state == "countdown" || state == "playing")) {
				Send(MatchMessage("forfeit", {{"reason", "openlr2_resume_unavailable"}}));
				status_ = "active play could not be restored";
			}
			return;
		}
		if (type == "replaced") {
			status_ = "connection moved to another client";
			return;
		}
		if (type == "error") {
			status_ = message.value("code", "Arena error") + ": "
				+ message.value("message", "");
			LogEvent("server_error", {
				{"code", message.value("code", "")},
				{"message", message.value("message", "")},
				{"match_id", message.value("match_id", "")},
			});
		}
	}

	void ReceiveArenaStatus(const nlohmann::json& message)
	{
		if (const auto player = message.find("player");
			player != message.end() && player->is_object()) {
			arenaRating_ = player->value(
				"rating_exact",
				player->value("rating", 1000.0));
			arenaMatchesPlayed_ = player->value("matches_played", 0);
			if (const auto queue = player->find("queue");
				queue != player->end() && queue->is_object()) {
				const std::string previous = queueStatus_;
				queueView_ = *queue;
				queueStatus_ = queue->value("status", "idle");
				roomCode_ = queue->value("room_code", roomCode_);
				config_.stayInRoom = queue->value("stay_in_room", config_.stayInRoom);
				if (queueStatus_ == "queued") {
					status_ = roomCode_.empty()
						? "waiting for opponent"
						: "in room; waiting for READY";
				}
				else if (queueStatus_ == "cancelled" || queueStatus_ == "idle") {
					status_ = "connected; F9 opens Arena controls";
				}
				if (previous != queueStatus_) {
					LogEvent("queue_status_changed", {
						{"from", previous},
						{"to", queueStatus_},
						{"room_code", roomCode_},
					});
				}
			}
		}
		publicRooms_ = message.value("public_rooms", nlohmann::json::array());
		rankingView_ = message.value("ranking", nlohmann::json::object());
		if (message.contains("lobby_chat")) {
			lobbyChat_ = message.value("lobby_chat", nlohmann::json::array());
			TrimChat(lobbyChat_);
		}
	}

	void ReceiveRoomStatus(const nlohmann::json& message)
	{
		const std::string incomingCode = message.value("room_code", "");
		if (!roomCode_.empty() && incomingCode != roomCode_) return;
		if (roomCode_.empty()) roomCode_ = incomingCode;
		roomView_ = message;
		if (const auto rules = message.find("rules");
			rules != message.end() && rules->is_object()) {
			ReceiveRoomSettings(*rules);
		}
		roomReady_ = false;
		for (const auto& player : message.value("players", nlohmann::json::array())) {
			if (player.value("player_id", 0) == config_.playerId) {
				roomReady_ = player.value("ready", false);
				break;
			}
		}
	}

	void ReceiveRoomSettings(const nlohmann::json& rules)
	{
		config_.room.roomName = rules.value("room_name", config_.room.roomName);
		config_.room.scoreRule = rules.value("score_rule", config_.room.scoreRule);
		config_.room.forcedGauge = rules.value("forced_gauge", config_.room.forcedGauge);
		config_.room.chartScope = rules.value("chart_scope", config_.room.chartScope);
		config_.room.nominationPolicy = rules.value(
			"nomination_policy",
			config_.room.nominationPolicy);
		config_.room.nominationSeconds = rules.value(
			"nomination_seconds",
			config_.room.nominationSeconds);
		config_.room.optionSeconds = rules.value(
			"option_seconds",
			config_.room.optionSeconds);
		config_.room.intermissionSeconds = rules.value(
			"intermission_seconds",
			config_.room.intermissionSeconds);
		config_.room.seriesFormat = rules.value(
			"series_format",
			config_.room.seriesFormat);
		config_.room.firstToWins = rules.value(
			"first_to_wins",
			config_.room.firstToWins);
		config_.room.spectatorPublic = rules.value(
			"spectator_public",
			config_.room.spectatorPublic);
		config_.room.forceHostOption = rules.value(
			"force_host_option",
			config_.room.forceHostOption);
	}

	void ReceiveMatchReserved(const nlohmann::json& message)
	{
		if (message.value("ruleset_profile", "") != kRulesetProfile) {
			status_ = "match ruleset mismatch";
			return;
		}
		const std::string incomingId = message.value("match_id", "");
		if (incomingId.empty()) return;
		matchId_ = incomingId;
		reserved_ = true;
		queueStatus_ = "reserved";
		phase_ = "nomination";
		status_ = "matched; waiting for nomination";
		resultVisible_ = false;
		resultView_ = nlohmann::json::object();
		ReceiveRules(message);
		ResetMatchTransient();
		LogEvent("match_reserved", {
			{"match_id", matchId_},
			{"ruleset_profile", std::string(kRulesetProfile)},
			{"forced_gauge", forcedGauge_},
			{"match_mode", matchMode_},
			{"room_code", roomCode_},
		});
	}

	void ReceiveRules(const nlohmann::json& message)
	{
		const auto rules = message.value("rules", nlohmann::json::object());
		if (!rules.is_object()) return;
		matchMode_ = rules.value("match_mode", matchMode_);
		scoreRule_ = rules.value("score_rule", scoreRule_);
		forcedGauge_ = rules.value("forced_gauge", forcedGauge_);
		roomCode_ = rules.value("room_code", roomCode_);
		roomHostId_ = rules.value("room_host_id", roomHostId_);
		selectorPlayerId_ = rules.value("selector_player_id", selectorPlayerId_);
		seriesFormat_ = rules.value("series_format", seriesFormat_);
		seriesRound_ = rules.value("series_round", seriesRound_);
	}

	void ReceiveNominationStatus(const nlohmann::json& message)
	{
		ReceiveRules(message);
		phase_ = "selecting";
		nominationView_ = message;
		nominationDeadline_ = message.contains("deadline")
			&& message["deadline"].is_number()
			? message["deadline"].get<double>()
			: 0.0;
		const bool canNominate = message.value("can_nominate", true);
		if (matchMode_ == "ranked") {
			if (!nominationSkipped_) {
				nominationSkipped_ = true;
				Send(MatchMessage("chart_nomination_skip"));
				status_ = "server-random nomination submitted";
			}
		}
		else {
			status_ = canNominate
				? "select a chart; Insert nominates / Delete random"
				: "waiting for the selector";
		}
	}

	void ReceiveChart(const nlohmann::json& message)
	{
		const auto chart = message.value("chart", nlohmann::json::object());
		chartHash_ = chart.value("md5", "");
		chartTitle_ = chart.value("title", "");
		chartTotalNotes_ = chart.value("totalnotes", 0);
		randomSeed_ = message.value("random_seed", 0L);
		playModeLabel_ = message.value("play_mode_label", "");
		SONGDATA song{};
		InitSongData(&song);
		chartAvailable_ = IsMd5(chartHash_)
			&& GetSongData(CSTR(chartHash_.c_str()), &song, database_, &game_->sSelect) == 1;
		playMode_ = chartAvailable_ ? song.keymode : 0;
		Send(MatchMessage("chart_check", {
			{"chart_hash", chartHash_},
			{"available", chartAvailable_},
			{"totalnotes", 0},
			{"play_mode", playMode_},
		}));
		phase_ = "chart check";
		status_ = chartAvailable_ ? "chart found locally" : "Arena chart missing";
		LogEvent("chart_check", {
			{"match_id", matchId_},
			{"chart_hash", chartHash_},
			{"available", chartAvailable_},
			{"play_mode", playMode_},
		});
	}

	void RespondToCpuChartRequest(const nlohmann::json& message)
	{
		phase_ = "cpu chart";
		nominationDeadline_ = message.contains("deadline")
			&& message["deadline"].is_number()
			? message["deadline"].get<double>()
			: 0.0;
		const auto candidates = message.value("candidates", nlohmann::json::array());
		const std::string selected = FirstOwnedCpuCandidate(
			candidates,
			[this](const std::string_view hash) { return SongOwned(hash); });
		if (!selected.empty()) {
			Send(MatchMessage("cpu_chart_candidate", {{"chart_hash", selected}}));
			status_ = "owned CPU chart submitted";
			return;
		}
		const bool more = message.value("has_more_candidates", false);
		const int nextOffset = message.value("next_candidate_offset", 0);
		if (more && nextOffset > 0) {
			Send(MatchMessage("cpu_chart_candidate", {
				{"chart_hash", ""},
				{"request_next_candidates", true},
				{"candidate_offset", nextOffset},
			}));
			status_ = "checking more CPU chart candidates";
			return;
		}
		Send(MatchMessage("cpu_chart_candidate", {{"chart_hash", ""}}));
		status_ = "no owned CPU chart in Arena tables";
	}

	bool SongOwned(const std::string_view hash) const
	{
		if (!database_ || !IsMd5(hash)) return false;
		sqlite3_stmt* statement = nullptr;
		if (sqlite3_prepare_v2(
				database_,
				"SELECT 1 FROM song WHERE hash=? LIMIT 1",
				-1,
				&statement,
				nullptr) != SQLITE_OK) {
			return false;
		}
		sqlite3_bind_text(
			statement,
			1,
			hash.data(),
			static_cast<int>(hash.size()),
			SQLITE_TRANSIENT);
		const bool owned = sqlite3_step(statement) == SQLITE_ROW;
		sqlite3_finalize(statement);
		return owned;
	}

	void ReceiveResult(const nlohmann::json& message)
	{
		ReceiveRules(message);
		resultView_ = message;
		resultVisible_ = true;
		liveView_ = message;
		bool autoReentered = false;
		for (const auto& id : message.value(
			"auto_reentry_player_ids",
			nlohmann::json::array())) {
			if (id.is_number_integer() && id.get<int>() == config_.playerId) {
				autoReentered = true;
				break;
			}
		}
		for (const auto& player : message.value("players", nlohmann::json::array())) {
			if (player.value("player_id", 0) != config_.playerId) continue;
			if (message.value("rated", false)) {
				lastRatingDelta_ = player.value("delta", 0.0);
				arenaRating_ = player.value("after", arenaRating_);
				arenaMatchesPlayed_++;
				ratingDeltaVisible_ = true;
			}
			break;
		}
		queueStatus_ = autoReentered ? "queued" : "cancelled";
		roomReady_ = false;
		status_ = autoReentered
			? (matchMode_ == "ranked"
				? "result; next rated match is queued"
				: "result; room retained, READY for next match")
			: "result; automatic entry ended";
		phase_ = "result";
		LogEvent("result_received", {
			{"match_id", matchId_},
			{"players", message.value("players", nlohmann::json::array()).size()},
			{"auto_reentered", autoReentered},
			{"rating_delta", lastRatingDelta_},
		});
		EndMatch(true);
		RequestStatus();
	}

	void UpdateClock(const nlohmann::json& message)
	{
		if (!message.contains("server_time")) return;
		const double now = EpochSeconds();
		const double sent = message.value("client_time", now);
		const double midpoint = sent + (now - sent) / 2.0;
		serverOffset_ = message.value("server_time", now) - midpoint;
	}

	void UpdateScene()
	{
		const std::string current = SceneName(
			game_->procSelecter,
			game_->gameplay.isAutoplay != 0,
			game_->gameplay.replay.status == 2,
			game_->gameplay.isCourse != 0);
		if (current == scene_) return;
		const std::string previous = scene_;
		scene_ = current;
		LogEvent("scene_changed", {
			{"from", previous},
			{"to", current},
			{"match_id", matchId_},
		});
		if (authenticated_) SendState();
		if (current == "result" && active_ && !finalSent_) SendFinal();
		if (current == "select" && active_ && !finalSent_ && previous == "play") {
			Send(MatchMessage("forfeit", {{"reason", "play_aborted"}}));
		}
		if (current == "select" && !active_) arenaChart_ = false;
		if (current != "select" && current != "result") {
			panelOpen_ = false;
			CloseTextInput();
		}
	}

	bool ConsumeKey(const int key)
	{
		if (game_->KeyInput.inputID[key] != 1) return false;
		game_->KeyInput.inputID[key] = 0;
		return true;
	}

	void HandleUiKeys()
	{
		if (ConsumeKey(kOverlayToggleKey)) {
			overlayVisible_ = !overlayVisible_;
			config_.showOverlay = overlayVisible_;
			WriteConfig();
		}
		if (textTarget_ != TextTarget::None) return;
		if (ConsumeKey(kPanelToggleKey)
			&& (scene_ == "select" || scene_ == "result")) {
			panelOpen_ = !panelOpen_;
			if (panelOpen_) RequestStatus();
		}
		if (!panelOpen_) return;
		if (ConsumeKey(KEY_INPUT_ESCAPE)) {
			panelOpen_ = false;
			return;
		}
		if (ConsumeKey(KEY_INPUT_TAB)) {
			panelPage_ = (panelPage_ + 1) % kPanelPageCount;
			menuIndex_ = 0;
			return;
		}
			const int itemCount = static_cast<int>(MenuItems().size());
		if (itemCount > 0) {
			if (ConsumeKey(KEY_INPUT_UP)) {
				menuIndex_ = menuIndex_ == 0 ? itemCount - 1 : menuIndex_ - 1;
			}
			if (ConsumeKey(KEY_INPUT_DOWN)) {
				menuIndex_ = (menuIndex_ + 1) % itemCount;
			}
			if (ConsumeKey(KEY_INPUT_LEFT)) AdjustMenu(-1);
			if (ConsumeKey(KEY_INPUT_RIGHT)) AdjustMenu(1);
			if (ConsumeKey(KEY_INPUT_RETURN)) ActivateMenu();
		}
	}

	void HandleArenaHotkeys()
	{
		if (!authenticated_) return;
		if (active_ && scene_ == "play" && ConsumeKey(kForceEndKey)) {
			if (!forceEndVoteSent_) {
				Send(MatchMessage("force_end_vote"));
				forceEndVoteSent_ = true;
				status_ = "force-end vote sent";
			}
			return;
		}
		if (game_->procSelecter != 2 || panelOpen_
			|| textTarget_ != TextTarget::None) return;
		if (ConsumeKey(KEY_INPUT_DELETE)
			&& reserved_ && phase_ == "selecting"
			&& matchMode_ != "ranked") {
			Send(MatchMessage("chart_nomination_skip"));
			status_ = "random nomination submitted";
			return;
		}
		if (!ConsumeKey(KEY_INPUT_INSERT)) return;
		if (reserved_ && phase_ == "options" && chartAvailable_
			&& !optionReadySent_) {
			SendOptionReady();
			return;
		}
		if (reserved_ && phase_ == "selecting" && matchMode_ != "ranked") {
			NominateCurrentChart();
			return;
		}
		if (!roomCode_.empty() && !active_) {
			RequestRoomReady(!roomReady_);
			return;
		}
		if (active_) return;
		RequestRatedToggle();
	}

	void SendOptionReady()
	{
		playOption_ = EncodePlayOption(
			playMode_,
			game_->config.play.random[PLAYER_1],
			game_->config.play.random[PLAYER_2],
			game_->config.play.dpFlip);
		if (Send(MatchMessage("option_ready", {
			{"play_option", playOption_},
			{"play_mode", playMode_},
		}))) {
			optionReadySent_ = true;
			status_ = "option locked";
			LogEvent("option_ready", {
				{"match_id", matchId_},
				{"play_mode", playMode_},
				{"play_option", playOption_},
			});
		}
	}

	void NominateCurrentChart()
	{
		if (!nominationView_.value("can_nominate", true)) {
			status_ = "this player is not the selector";
			return;
		}
		if (game_->sSelect.cur_song < 0) return;
		const auto& song = game_->sSelect.bmsList[game_->sSelect.cur_song];
		const std::string hash = song.hash.body;
		if (song.keymode <= 0 || !IsMd5(hash)) {
			status_ = "select a playable BMS chart";
			return;
		}
		Send(MatchMessage("chart_nominate", {{"chart_hash", hash}}));
		status_ = "nomination submitted";
	}

	void RequestRatedToggle()
	{
		if (queueStatus_ == "idle" || queueStatus_.empty()
			|| queueStatus_ == "cancelled") {
			Send({
				{"type", "queue_entry"},
				{"ruleset_profile", std::string(kRulesetProfile)},
				{"unrestricted_rating", config_.unrestrictedRating},
				{"allow_cpu", config_.allowCpu},
			});
			status_ = "rated entry requested";
		}
		else {
			Send({{"type", "queue_cancel"}});
			status_ = "leave requested";
		}
	}

	void RequestRoomEntry(const std::string& code, const std::string& password)
	{
		nlohmann::json message = {
			{"type", "room_entry"},
			{"match_mode", "private"},
			{"score_rule", config_.room.scoreRule},
			{"forced_gauge", config_.room.forcedGauge},
			{"chart_scope", config_.room.chartScope},
			{"room_code", code},
			{"room_name", config_.room.roomName},
			{"room_password", password},
				{"ruleset_profile", std::string(kRulesetProfile)},
			{"nomination_policy", config_.room.nominationPolicy},
			{"nomination_seconds", config_.room.nominationSeconds},
			{"option_seconds", config_.room.optionSeconds},
			{"intermission_seconds", config_.room.intermissionSeconds},
			{"series_format", config_.room.seriesFormat},
			{"first_to_wins", config_.room.firstToWins},
			{"stay_in_room", config_.stayInRoom},
			{"room_participating", true},
			{"spectator_public", config_.room.spectatorPublic},
			{"force_host_option", config_.room.forceHostOption},
		};
		Send(message);
		roomReady_ = false;
		status_ = code.empty() ? "creating room" : "joining room";
	}

	void RequestRoomSettings()
	{
		if (!IsRoomHost()) return;
		Send({
			{"type", "room_settings"},
			{"score_rule", config_.room.scoreRule},
			{"forced_gauge", config_.room.forcedGauge},
			{"chart_scope", config_.room.chartScope},
			{"room_name", config_.room.roomName},
			{"room_password", pendingRoomPassword_},
			{"update_password", roomPasswordDirty_},
			{"nomination_policy", config_.room.nominationPolicy},
			{"nomination_seconds", config_.room.nominationSeconds},
			{"option_seconds", config_.room.optionSeconds},
			{"intermission_seconds", config_.room.intermissionSeconds},
			{"series_format", config_.room.seriesFormat},
			{"first_to_wins", config_.room.firstToWins},
			{"ruleset_profile", std::string(kRulesetProfile)},
			{"spectator_public", config_.room.spectatorPublic},
			{"force_host_option", config_.room.forceHostOption},
		});
		roomPasswordDirty_ = false;
		pendingRoomPassword_.clear();
		WriteConfig();
		status_ = "room settings submitted";
	}

	void RequestRoomReady(const bool ready)
	{
		roomReady_ = ready;
		Send({{"type", "room_ready"}, {"ready", ready}});
		SendState();
		status_ = ready ? "room READY" : "room READY cancelled";
	}

	void RequestStatus()
	{
		if (authenticated_) Send({{"type", "arena_status"}});
	}

	void RequestManual()
	{
		if (authenticated_) Send({{"type", "arena_manual"}});
	}

	void SendLiveIfDue()
	{
		if (!active_ || scene_ != "play" || startAt_ <= 0.0
			|| ServerNow() < startAt_) return;
		const auto now = Clock::now();
		if (now - lastLive_ < std::chrono::seconds(1)) return;
		lastLive_ = now;
		const auto& player = game_->gameplay.player[PLAYER_1];
		const int totalNotes = chartTotalNotes_ > 0
			? chartTotalNotes_
			: std::max(player.totalnotes, 0);
		const int processed = std::clamp(player.note_current, 0, totalNotes);
		const int maxCombo = std::clamp(player.max_combo, 0, processed);
		Send(MatchMessage("live", {
			{"seq", ++sequence_},
			{"exscore", std::max(player.exscore, 0)},
			{"processed_notes", processed},
			{"minbp", std::max(player.judgecount[1], 0) + std::max(player.judgecount[2], 0)},
			{"max_combo", maxCombo},
			{"play_option", playOption_},
			{"play_mode", playMode_},
			{"ln_mode", "LN"},
		}));
	}

	void SendFinal()
	{
		const auto& player = game_->gameplay.player[PLAYER_1];
		const bool failed = player.clearType <= 1;
		const int totalNotes = chartTotalNotes_ > 0
			? chartTotalNotes_
			: std::max(player.totalnotes, 0);
		const int processed = failed
			? std::clamp(player.note_current, 0, totalNotes)
			: totalNotes;
		const int maxCombo = std::clamp(player.max_combo, 0, processed);
		const int clearType = ArenaClearType(
			player.clearType,
			player.gaugeType,
			player.exscore,
			totalNotes,
			player.judgecount[3]);
		finalSent_ = true;
		Send(MatchMessage("final", {
			{"seq", ++sequence_},
			{"exscore", std::max(player.exscore, 0)},
			{"processed_notes", processed},
			{"minbp", std::max(player.judgecount[1], 0) + std::max(player.judgecount[2], 0)},
			{"max_combo", maxCombo},
			{"state", failed ? "hard_fail" : "complete"},
			{"clear_type", failed ? 1 : clearType},
			{"play_option", playOption_},
			{"play_mode", playMode_},
			{"ln_mode", "LN"},
		}));
		active_ = false;
		status_ = "result sent; waiting for Arena result";
		LogEvent("final_sent", {
			{"match_id", matchId_},
			{"chart_hash", chartHash_},
			{"state", failed ? "hard_fail" : "complete"},
			{"exscore", std::max(player.exscore, 0)},
			{"processed_notes", processed},
		});
	}

	void SendState()
	{
		const bool baseReady = (scene_ == "select" || scene_ == "result");
		const bool ready = !roomCode_.empty()
			? baseReady && roomReady_
			: reserved_ && !active_ && baseReady;
		Send({
			{"type", "state"},
			{"state", scene_},
			{"arena_enabled", true},
			{"ready", ready},
		});
	}

	nlohmann::json MatchMessage(
		const std::string_view type,
		nlohmann::json details = {}) const
	{
		nlohmann::json result = {
			{"type", std::string(type)},
			{"match_id", matchId_},
		};
		if (details.is_object()) result.update(details);
		return result;
	}

	bool Send(const nlohmann::json& message)
	{
		LogMessage("outbound", message);
		if (transport_.Send(message.dump())) return true;
		LogEvent("send_failed", {
			{"type", message.value("type", "")},
			{"match_id", message.value("match_id", "")},
			{"error", transport_.LastError()},
		});
		return false;
	}

	void ResetMatchTransient()
	{
		nominationSkipped_ = false;
		optionReadySent_ = false;
		chartAvailable_ = false;
		launchRequested_ = false;
		playReadySent_ = false;
		finalSent_ = false;
		forceEndVoteSent_ = false;
		sequence_ = 0;
		startAt_ = 0.0;
		loadDeadline_ = 0.0;
		fillDeadline_ = 0.0;
		nominationDeadline_ = 0.0;
		optionDeadline_ = 0.0;
		randomSeed_ = 0;
		chartHash_.clear();
		chartTitle_.clear();
		playModeLabel_.clear();
		chartTotalNotes_ = 0;
		playMode_ = 0;
		playOption_ = 0;
		liveView_ = nlohmann::json::object();
		nominationView_ = nlohmann::json::object();
	}

	void EndMatch(const bool keepResult)
	{
		active_ = false;
		reserved_ = false;
		launchRequested_ = false;
		startAt_ = 0.0;
		loadDeadline_ = 0.0;
		fillDeadline_ = 0.0;
		nominationDeadline_ = 0.0;
		optionDeadline_ = 0.0;
		matchId_.clear();
		if (!keepResult) {
			resultView_ = nlohmann::json::object();
			resultVisible_ = false;
			ratingDeltaVisible_ = false;
		}
		RestoreOptions();
	}

	void RestoreOptions()
	{
		if (game_ && savedPlayConfig_) {
			game_->config.play = *savedPlayConfig_;
			savedPlayConfig_.reset();
			LogEvent("play_options_restored");
		}
	}

	static void TrimChat(nlohmann::json& messages)
	{
		if (!messages.is_array()) messages = nlohmann::json::array();
		while (messages.size() > kMaxChatMessages) messages.erase(messages.begin());
	}

	static void AppendChat(nlohmann::json& messages, const nlohmann::json& message)
	{
		if (!messages.is_array()) messages = nlohmann::json::array();
		messages.push_back(message);
		TrimChat(messages);
	}

	bool IsRoomHost() const
	{
		if (roomHostId_ > 0) return roomHostId_ == config_.playerId;
		const auto rules = roomView_.value("rules", nlohmann::json::object());
		return rules.value("room_host_id", 0) == config_.playerId;
	}

	std::vector<std::string> MenuItems() const
	{
		std::vector<std::string> items;
		switch (panelPage_) {
			case 0:
				items = {
					queueStatus_ == "idle" || queueStatus_ == "cancelled"
						? "Enter rated Arena"
						: "Leave current queue / room",
					std::string("CPU match: ") + (config_.allowCpu ? "ON" : "OFF"),
					std::string("Unrestricted rating: ")
						+ (config_.unrestrictedRating ? "ON" : "OFF"),
					std::string("Stay in room after match: ")
						+ (config_.stayInRoom ? "ON" : "OFF"),
					std::string("Room READY: ") + (roomReady_ ? "ON" : "OFF"),
					std::string("Participating: ") + (IsParticipating() ? "ON" : "SPECTATE"),
					"Close result",
					"Vote to force-end current chart (End during play)",
					"Copy current room code",
					"Refresh Arena status",
				};
				break;
			case 1:
				for (const auto& room : publicRooms_) {
					if (!room.is_object()) continue;
					items.push_back(
						room.value("room_code", "") + "  "
						+ DisplayString(room.value("room_name", "Arena room"))
						+ "  " + std::to_string(room.value("member_count", 0))
						+ "/8"
						+ (room.value("locked", false) ? " [LOCKED]" : ""));
				}
				if (items.empty()) items.push_back("No public rooms (refresh)");
				items.push_back("Join by room code");
				items.push_back("Join by clipboard room code");
				break;
			case 2:
				items = {
					"Create room with these settings",
					"Room name: " + DisplayString(config_.room.roomName),
					"Room password: " + std::string(roomPasswordDirty_ ? "[CHANGED]" : "[UNCHANGED]"),
					"Score rule: " + config_.room.scoreRule,
					"Forced gauge: " + config_.room.forcedGauge,
					"Chart scope: " + config_.room.chartScope,
					"Nomination: " + config_.room.nominationPolicy,
					"Nomination seconds: " + std::to_string(config_.room.nominationSeconds),
					"Option seconds: " + std::to_string(config_.room.optionSeconds),
					"Intermission seconds: " + std::to_string(config_.room.intermissionSeconds),
					"Series: " + config_.room.seriesFormat,
					"First-to wins: " + std::to_string(config_.room.firstToWins),
					std::string("Public/listed spectators: ")
						+ (config_.room.spectatorPublic ? "ON" : "OFF"),
					std::string("Force host lane option: ")
						+ (config_.room.forceHostOption ? "ON" : "OFF"),
					"Apply settings to current room",
					"Target member: " + TargetPlayerLabel(),
					"Kick target",
					"Transfer host to target",
					"Make target the selector",
					"Disband room",
				};
				break;
			case 3:
				items = {
					"Send public lobby chat",
					"Send match / room chat",
				};
				break;
			case 4:
				items = {
					"Previous manual section",
					"Next manual section",
					"Reload manual from server",
				};
				break;
			default:
				break;
		}
		return items;
	}

	void AdjustMenu(const int direction)
	{
		if (panelPage_ == 4) {
			AdjustManualSection(direction);
			return;
		}
		if (panelPage_ != 2) return;
		switch (menuIndex_) {
			case 3:
				CycleValue(
					config_.room.scoreRule,
					std::vector<std::string>{"exscore", "minbp", "max_combo"},
					direction);
				break;
			case 4:
				CycleValue(
					config_.room.forcedGauge,
					std::vector<std::string>{"free", "normal", "hard", "exhard", "hazard"},
					direction);
				break;
			case 5:
				CycleValue(
					config_.room.chartScope,
					std::vector<std::string>{"official", "free"},
					direction);
				break;
			case 6:
				CycleValue(
					config_.room.nominationPolicy,
					std::vector<std::string>{"all", "host", "rotate"},
					direction);
				break;
			case 7:
				config_.room.nominationSeconds = std::clamp(
					config_.room.nominationSeconds + direction * 10,
					10,
					180);
				break;
			case 8:
				config_.room.optionSeconds = std::clamp(
					config_.room.optionSeconds + direction * 5,
					5,
					60);
				break;
			case 9:
				config_.room.intermissionSeconds = std::clamp(
					config_.room.intermissionSeconds + direction * 5,
					0,
					60);
				break;
			case 10:
				CycleValue(
					config_.room.seriesFormat,
					std::vector<std::string>{"single", "all_picks", "first_to"},
					direction);
				break;
			case 11:
				config_.room.firstToWins = std::clamp(
					config_.room.firstToWins + direction,
					2,
					5);
				break;
			case 12:
				config_.room.spectatorPublic = !config_.room.spectatorPublic;
				break;
			case 13:
				config_.room.forceHostOption = !config_.room.forceHostOption;
				break;
			case 15:
				AdjustTargetPlayer(direction);
				break;
			default:
				return;
		}
		WriteConfig();
	}

	void ActivateMenu()
	{
		if (panelPage_ == 0) ActivateMainMenu();
		else if (panelPage_ == 1) ActivatePublicRoomMenu();
		else if (panelPage_ == 2) ActivateRoomSettingsMenu();
		else if (panelPage_ == 3) ActivateChatMenu();
		else if (panelPage_ == 4) {
			if (menuIndex_ == 0) AdjustManualSection(-1);
			else if (menuIndex_ == 1) AdjustManualSection(1);
			else RequestManual();
		}
	}

	void ActivateMainMenu()
	{
		switch (menuIndex_) {
			case 0:
				if (!roomCode_.empty() || (queueStatus_ != "idle" && queueStatus_ != "cancelled")) {
					Send({{"type", "queue_cancel"}});
					status_ = "leave requested";
				}
				else RequestRatedToggle();
				break;
			case 1:
				config_.allowCpu = !config_.allowCpu;
				WriteConfig();
				break;
			case 2:
				config_.unrestrictedRating = !config_.unrestrictedRating;
				WriteConfig();
				break;
			case 3:
				config_.stayInRoom = !config_.stayInRoom;
				WriteConfig();
				if (!roomCode_.empty()) {
					Send({
						{"type", "room_stay"},
						{"stay_in_room", config_.stayInRoom},
					});
				}
				break;
			case 4:
				if (!roomCode_.empty()) RequestRoomReady(!roomReady_);
				break;
			case 5:
				if (!roomCode_.empty()) {
					Send({
						{"type", "room_participation"},
						{"participating", !IsParticipating()},
					});
				}
				break;
			case 6:
				resultVisible_ = false;
				ratingDeltaVisible_ = false;
				break;
			case 7:
				if (active_ && !forceEndVoteSent_) {
					Send(MatchMessage("force_end_vote"));
					forceEndVoteSent_ = true;
				}
				break;
			case 8:
				CopyCurrentRoomCode();
				break;
			case 9:
				RequestStatus();
				break;
			default:
				break;
		}
	}

	void ActivatePublicRoomMenu()
	{
		const int roomCount = static_cast<int>(publicRooms_.size());
		if (menuIndex_ < roomCount) {
			const auto& room = publicRooms_[menuIndex_];
			pendingJoinCode_ = NormalizeRoomCode(room.value("room_code", ""));
			if (pendingJoinCode_.empty()) return;
			if (room.value("locked", false)) {
				StartTextInput(TextTarget::JoinRoomPassword, "", 64);
			}
			else {
				RequestRoomEntry(pendingJoinCode_, "");
			}
			return;
		}
		if (menuIndex_ == roomCount) {
			StartTextInput(TextTarget::JoinRoomCode, "", 6);
			return;
		}
		if (menuIndex_ == roomCount + 1) {
			const std::string code = NormalizeRoomCode(ReadClipboardText());
			if (code.empty()) {
				status_ = "clipboard does not contain a room code";
				return;
			}
			pendingJoinCode_ = code;
			StartTextInput(TextTarget::JoinRoomPassword, "", 64);
		}
	}

	void ActivateRoomSettingsMenu()
	{
		switch (menuIndex_) {
			case 0:
				RequestRoomEntry("", pendingRoomPassword_);
				break;
			case 1:
				StartTextInput(
					TextTarget::RoomName,
					config_.room.roomName,
					40);
				break;
			case 2:
				StartTextInput(TextTarget::RoomPassword, "", 64);
				break;
			case 3:
			case 4:
			case 5:
			case 6:
			case 7:
			case 8:
			case 9:
			case 10:
			case 11:
			case 12:
			case 13:
			case 15:
				AdjustMenu(1);
				break;
			case 14:
				RequestRoomSettings();
				break;
			case 16:
				SendRoomPlayerAction("room_kick");
				break;
			case 17:
				SendRoomPlayerAction("room_transfer_host");
				break;
			case 18:
				SendRoomPlayerAction("room_set_selector");
				break;
			case 19:
				if (IsRoomHost()) Send({{"type", "room_disband"}});
				break;
			default:
				break;
		}
	}

	void ActivateChatMenu()
	{
		if (menuIndex_ == 0) StartTextInput(TextTarget::LobbyChat, "", 200);
		else if (menuIndex_ == 1) StartTextInput(TextTarget::RoomChat, "", 200);
	}

	void SendRoomPlayerAction(const std::string_view type)
	{
		if (!IsRoomHost()) return;
		const int target = TargetPlayerId();
		if (target <= 0 || target == config_.playerId) return;
		Send({
			{"type", type},
			{"player_id", target},
		});
	}

	bool IsParticipating() const
	{
		for (const auto& player : roomView_.value(
			"players",
			nlohmann::json::array())) {
			if (player.value("player_id", 0) == config_.playerId) {
				return player.value("participating", true);
			}
		}
		return true;
	}

	int TargetPlayerId() const
	{
		const auto players = roomView_.value("players", nlohmann::json::array());
		if (!players.is_array() || players.empty()) return 0;
		const int index = std::clamp(
			targetPlayerIndex_,
			0,
			static_cast<int>(players.size()) - 1);
		return players[index].value("player_id", 0);
	}

	std::string TargetPlayerLabel() const
	{
		const auto players = roomView_.value("players", nlohmann::json::array());
		if (!players.is_array() || players.empty()) return "-";
		const int index = std::clamp(
			targetPlayerIndex_,
			0,
			static_cast<int>(players.size()) - 1);
		return DisplayString(players[index].value("name", "-"));
	}

	void AdjustTargetPlayer(const int direction)
	{
		const auto players = roomView_.value("players", nlohmann::json::array());
		if (!players.is_array() || players.empty()) {
			targetPlayerIndex_ = 0;
			return;
		}
		targetPlayerIndex_ += direction;
		if (targetPlayerIndex_ < 0) {
			targetPlayerIndex_ = static_cast<int>(players.size()) - 1;
		}
		else if (targetPlayerIndex_ >= static_cast<int>(players.size())) {
			targetPlayerIndex_ = 0;
		}
	}

	void AdjustManualSection(const int direction)
	{
		const auto sections = manualView_.value("sections", nlohmann::json::array());
		if (!sections.is_array() || sections.empty()) return;
		manualSection_ += direction;
		if (manualSection_ < 0) manualSection_ = static_cast<int>(sections.size()) - 1;
		else if (manualSection_ >= static_cast<int>(sections.size())) manualSection_ = 0;
	}

	void StartTextInput(
		const TextTarget target,
		const std::string& initialUtf8,
		const int maximum)
	{
		CloseTextInput();
		InitKeyInput();
		textInputHandle_ = MakeKeyInput(maximum, 1, 0, 0);
		if (textInputHandle_ < 0) {
			status_ = "text input could not be opened";
			return;
		}
		const std::string initial = DisplayString(initialUtf8);
		SetKeyInputString(initial.c_str(), textInputHandle_);
		SetActiveKeyInput(textInputHandle_);
		textTarget_ = target;
	}

	void PollTextInput()
	{
		if (textTarget_ == TextTarget::None || textInputHandle_ < 0) return;
		const int state = CheckKeyInput(textInputHandle_);
		if (state <= 0) return;
		std::string value;
		const TextTarget target = textTarget_;
		if (state == 1) {
			char buffer[2048]{};
			GetKeyInputString(buffer, textInputHandle_);
			value = ansi2utf(buffer, 932);
		}
		CloseTextInput();
		if (state != 1) return;
		switch (target) {
			case TextTarget::JoinRoomCode:
				pendingJoinCode_ = NormalizeRoomCode(value);
				if (pendingJoinCode_.empty()) {
					status_ = "invalid room code";
				}
				else {
					StartTextInput(TextTarget::JoinRoomPassword, "", 64);
				}
				break;
			case TextTarget::JoinRoomPassword:
				RequestRoomEntry(pendingJoinCode_, value);
				break;
			case TextTarget::RoomName:
				config_.room.roomName = value;
				WriteConfig();
				break;
			case TextTarget::RoomPassword:
				pendingRoomPassword_ = value;
				roomPasswordDirty_ = true;
				break;
			case TextTarget::RoomChat:
				if (!value.empty() && (!roomCode_.empty() || reserved_)) {
					nlohmann::json chat = {
						{"type", "chat_send"},
						{"text", value},
					};
					if (!matchId_.empty()) chat["match_id"] = matchId_;
					if (!roomCode_.empty()) chat["room_code"] = roomCode_;
					Send(chat);
				}
				break;
			case TextTarget::LobbyChat:
				if (!value.empty()) Send({{"type", "lobby_chat_send"}, {"text", value}});
				break;
			default:
				break;
		}
	}

	void CloseTextInput()
	{
		if (textInputHandle_ >= 0) DeleteKeyInput(textInputHandle_);
		textInputHandle_ = -1;
		textTarget_ = TextTarget::None;
	}

	static std::string ReadClipboardText()
	{
		char buffer[2048]{};
		if (GetClipboardText(buffer, static_cast<int>(sizeof(buffer))) < 0) {
			return {};
		}
		return ansi2utf(buffer, 932);
	}

	void CopyCurrentRoomCode()
	{
		if (roomCode_.empty()) {
			status_ = "not in a room";
			return;
		}
		if (SetClipboardText(roomCode_.c_str()) == 0) {
			status_ = "room code copied: " + roomCode_;
		}
		else {
			status_ = "room code copy failed";
		}
	}

	std::tuple<std::string, long long, bool> PhaseActionAndCountdown() const
	{
		double deadline = 0.0;
		std::string action;
		if (phase_ == "filling") {
			action = "READY / waiting for more participants";
			deadline = fillDeadline_;
		}
		else if (phase_ == "selecting" || phase_ == "nomination"
			|| phase_ == "cpu chart") {
			action = phase_ == "cpu chart"
				? "CPU is selecting an owned chart"
				: "Select / nominate a chart";
			deadline = nominationDeadline_;
		}
		else if (phase_ == "options") {
			action = optionReadySent_
				? "Waiting for other players' options"
				: "Choose options and press Insert";
			deadline = optionDeadline_;
		}
		else if (phase_ == "loading") {
			action = "Loading chart / waiting for players";
			deadline = loadDeadline_;
		}
		else if (phase_ == "countdown") {
			action = "Arena match starts";
			deadline = startAt_;
		}
		else if (phase_ == "playing") action = "Playing (End: force-end vote)";
		else if (resultVisible_) action = "Match result (F9: close / next controls)";
		else if (!roomCode_.empty()) {
			action = roomView_.value("paused", false)
				? "Room break: everyone is spectating"
				: "Room lobby: press READY when prepared";
		}
		else if (queueStatus_ == "queued") action = "Waiting for Arena match";
		else action = "Open F9 controls to enter Arena or a room";
		if (!playModeLabel_.empty()
			&& (reserved_ || active_ || resultVisible_)) {
			action += " / " + playModeLabel_;
		}
		return {
			action,
			CountdownSeconds(deadline, ServerNow()),
			deadline > 0.0,
		};
	}

	static int CountdownColor(const long long seconds)
	{
		switch (CountdownColorBand(seconds)) {
			case CountdownBand::Red:
				return GetColor(255, 80, 80);
			case CountdownBand::Yellow:
				return GetColor(255, 225, 80);
			default:
				return GetColor(190, 220, 255);
		}
	}

	int DrawBattle(const int startY) const
	{
		const auto players = liveView_.value("players", nlohmann::json::array());
		if (!players.is_array() || players.empty()) return startY;
		const int height = 34 + static_cast<int>(players.size()) * 28;
		SetDrawBlendMode(DX_BLENDMODE_ALPHA, 210);
		DrawBox(8, startY, 862, startY + height, GetColor(12, 12, 18), TRUE);
		SetDrawBlendMode(DX_BLENDMODE_NOBLEND, 255);
		const auto chart = liveView_.value("chart", nlohmann::json::object());
		const std::string title = DisplayString(chart.value("title", chartTitle_));
		DrawFormatString(
			16,
			startY + 6,
			GetColor(235, 235, 240),
			"%s / %s / %s",
			scoreRule_.c_str(),
			liveView_.value("play_mode_label", "").c_str(),
			title.c_str());
		int y = startY + 28;
		for (const auto& player : players) {
			const std::string name = DisplayString(player.value("name", "-"));
			double rate = player.value("battle_rate", -1.0);
			if (rate < 0.0) {
				const double maximum = std::max(
					1.0,
					player.value("battle_max", 1.0));
				rate = player.value("battle_value", 0.0) / maximum;
			}
			rate = std::clamp(rate, 0.0, 1.0);
			const int x1 = 220;
			const int x2 = 840;
			DrawBox(x1, y + 2, x2, y + 18, GetColor(45, 45, 55), TRUE);
			DrawBox(
				x1,
				y + 2,
				x1 + static_cast<int>((x2 - x1) * rate),
				y + 18,
				GetColor(80, 170, 255),
				TRUE);
			DrawFormatString(
				16,
				y,
				GetColor(230, 230, 235),
				"%-18.18s EX:%d BP:%d COMBO:%d%s",
				name.c_str(),
				player.value("exscore", 0),
				player.value("minbp", 0),
				player.value("max_combo", 0),
				player.value("finished", false) ? " DONE" : "");
			y += 28;
		}
		return startY + height;
	}

	int DrawResult(const int startY) const
	{
		const auto players = resultView_.value("players", nlohmann::json::array());
		const int height = 58 + static_cast<int>(players.size()) * 22;
		SetDrawBlendMode(DX_BLENDMODE_ALPHA, 225);
		DrawBox(8, startY, 862, startY + height, GetColor(16, 14, 20), TRUE);
		SetDrawBlendMode(DX_BLENDMODE_NOBLEND, 255);
		DrawString(16, startY + 7, "Arena result (F9 > MAIN > Close result)", GetColor(255, 220, 170));
		if (ratingDeltaVisible_) {
			DrawFormatString(
				560,
				startY + 4,
				lastRatingDelta_ > 0
					? GetColor(100, 255, 140)
					: lastRatingDelta_ < 0
						? GetColor(255, 100, 100)
						: GetColor(230, 230, 230),
				"RATING %+.0f",
				lastRatingDelta_);
		}
		int y = startY + 30;
		for (const auto& player : players) {
			const std::string name = DisplayString(player.value("name", "-"));
			DrawFormatString(
				16,
				y,
				GetColor(230, 230, 235),
				"#%d %-18.18s EX:%d BP:%d COMBO:%d %s",
				player.value("placement", 0),
				name.c_str(),
				player.value("exscore", 0),
				player.value("minbp", 0),
				player.value("max_combo", 0),
				player.value("clear_label", "").c_str());
			y += 22;
		}
		return startY + height;
	}

	void DrawPanel(const int startY) const
	{
		int screenWidth = 1280;
		int screenHeight = 720;
		GetDrawScreenSize(&screenWidth, &screenHeight);
		const int bottom = std::min(screenHeight - 8, startY + 520);
		SetDrawBlendMode(DX_BLENDMODE_ALPHA, 235);
		DrawBox(8, startY, std::min(1010, screenWidth - 8), bottom, GetColor(8, 8, 13), TRUE);
		SetDrawBlendMode(DX_BLENDMODE_NOBLEND, 255);
		const char* pages[] = {"MAIN", "PUBLIC ROOMS", "ROOM SETUP", "CHAT", "MANUAL"};
		DrawFormatString(
			16,
			startY + 8,
			GetColor(255, 235, 170),
			"Arena control / %s   Tab: page  arrows: move/change  Enter: action  Esc: close",
			pages[panelPage_]);
		if (textTarget_ != TextTarget::None && textInputHandle_ >= 0) {
			DrawString(16, startY + 38, "Enter text and press Enter (Esc cancels):", GetColor(230, 230, 235));
			DrawKeyInputString(16, startY + 64, textInputHandle_, TRUE);
			return;
		}
		if (panelPage_ == 3) DrawChat(startY + 38);
		else if (panelPage_ == 4) DrawManual(startY + 38);
		const auto items = MenuItems();
		int y = panelPage_ == 3 ? startY + 330
			: panelPage_ == 4 ? startY + 330
			: startY + 38;
		for (int index = 0;
			index < static_cast<int>(items.size()) && y < bottom - 18;
			index++, y += 21) {
			DrawFormatString(
				16,
				y,
				index == menuIndex_
					? GetColor(120, 220, 255)
					: GetColor(220, 220, 228),
				"%s %s",
				index == menuIndex_ ? ">" : " ",
				items[index].c_str());
		}
	}

	void DrawChat(const int startY) const
	{
		DrawString(16, startY, "Public lobby chat (latest 20)", GetColor(180, 220, 255));
		int y = startY + 22;
		const int first = lobbyChat_.is_array()
			? std::max(0, static_cast<int>(lobbyChat_.size()) - 8)
			: 0;
		for (int index = first;
			lobbyChat_.is_array() && index < static_cast<int>(lobbyChat_.size());
			index++, y += 18) {
			const auto& message = lobbyChat_[index];
			const std::string name = DisplayString(message.value("name", "-"));
			const std::string text = DisplayString(message.value("text", ""));
			DrawFormatString(16, y, GetColor(215, 215, 220), "%s: %.120s", name.c_str(), text.c_str());
		}
		y += 8;
		DrawString(16, y, "Room / match chat", GetColor(180, 220, 255));
		y += 22;
		const int roomFirst = matchChat_.is_array()
			? std::max(0, static_cast<int>(matchChat_.size()) - 5)
			: 0;
		for (int index = roomFirst;
			matchChat_.is_array() && index < static_cast<int>(matchChat_.size());
			index++, y += 18) {
			const auto& message = matchChat_[index];
			const std::string name = DisplayString(message.value("name", "-"));
			const std::string text = DisplayString(message.value("text", ""));
			DrawFormatString(16, y, GetColor(215, 215, 220), "%s: %.120s", name.c_str(), text.c_str());
		}
	}

	void DrawManual(const int startY) const
	{
		const auto sections = manualView_.value("sections", nlohmann::json::array());
		if (!sections.is_array() || sections.empty()) {
			DrawString(16, startY, "Manual is not available. Select Reload.", GetColor(230, 210, 160));
			return;
		}
		const int index = std::clamp(
			manualSection_,
			0,
			static_cast<int>(sections.size()) - 1);
		const auto& section = sections[index];
		const std::string title = DisplayString(section.value("title", "Arena"));
		DrawFormatString(
			16,
			startY,
			GetColor(180, 220, 255),
			"[%d/%d] %s",
			index + 1,
			static_cast<int>(sections.size()),
			title.c_str());
		int y = startY + 26;
		for (const auto& item : section.value("items", nlohmann::json::array())) {
			if (!item.is_string() || y > startY + 250) break;
			const std::string text = DisplayString(item.get<std::string>());
			DrawFormatString(16, y, GetColor(220, 220, 225), "- %.145s", text.c_str());
			y += 24;
		}
	}

	game* game_{};
	sqlite3* database_{};
	ArenaConfig config_;
	WebSocketTransport transport_;
	std::atomic_bool shutdown_{false};
	bool transportWasConnected_{};
	bool helloSent_{};
	bool authenticated_{};
	bool reserved_{};
	bool nominationSkipped_{};
	bool optionReadySent_{};
	bool chartAvailable_{};
	bool launchRequested_{};
	std::atomic_bool playReadySent_{false};
	bool finalSent_{};
	bool forceEndVoteSent_{};
	bool arenaChart_{};
	std::atomic_bool active_{false};
	std::atomic<double> startAt_{0.0};
	std::atomic<double> loadDeadline_{0.0};
	std::atomic<double> serverOffset_{0.0};
	double fillDeadline_{};
	double nominationDeadline_{};
	double optionDeadline_{};
	std::string scene_{"unknown"};
	std::string status_{"disabled"};
	std::string phase_{"idle"};
	std::string queueStatus_{"idle"};
	std::string matchId_;
	std::string roomCode_;
	std::string chartHash_;
	std::string chartTitle_;
	std::string playModeLabel_;
	std::string forcedGauge_{"free"};
	std::string scoreRule_{"exscore"};
	std::string matchMode_{"ranked"};
	std::string seriesFormat_{"single"};
	std::string lastTransportError_;
	std::string pendingJoinCode_;
	std::string pendingRoomPassword_;
	int chartTotalNotes_{};
	int playMode_{};
	int playOption_{};
	int arenaMatchesPlayed_{};
	int roomHostId_{};
	int selectorPlayerId_{};
	int seriesRound_{1};
	int panelPage_{};
	int menuIndex_{};
	int manualSection_{};
	int targetPlayerIndex_{};
	int textInputHandle_{-1};
	TextTarget textTarget_{TextTarget::None};
	long long randomSeed_{};
	long long sequence_{};
	double arenaRating_{1000.0};
	double lastRatingDelta_{};
	bool roomReady_{};
	bool roomPasswordDirty_{};
	bool resultVisible_{};
	bool ratingDeltaVisible_{};
	bool panelOpen_{};
	bool overlayVisible_{true};
	nlohmann::json queueView_{nlohmann::json::object()};
	nlohmann::json roomView_{nlohmann::json::object()};
	nlohmann::json liveView_{nlohmann::json::object()};
	nlohmann::json resultView_{nlohmann::json::object()};
	nlohmann::json nominationView_{nlohmann::json::object()};
	nlohmann::json publicRooms_{nlohmann::json::array()};
	nlohmann::json rankingView_{nlohmann::json::object()};
	nlohmann::json lobbyChat_{nlohmann::json::array()};
	nlohmann::json matchChat_{nlohmann::json::array()};
	nlohmann::json manualView_{nlohmann::json::object()};
	std::optional<CONFIG_PLAY> savedPlayConfig_;
	Clock::time_point lastPing_{};
	Clock::time_point lastLive_{};
	Clock::time_point lastConnectAttempt_{};
};

std::unique_ptr<Client> g_client;

} // namespace

void Initialize(game* gameState)
{
	g_client = std::make_unique<Client>(gameState);
}

void Shutdown()
{
	if (g_client) g_client->Shutdown();
	g_client.reset();
}

void Tick(game* gameState, sqlite3* songDatabase)
{
	if (g_client) g_client->Tick(gameState, songDatabase);
}

void DrawOverlay(const game* gameState)
{
	if (g_client) g_client->Draw(gameState);
}

bool ConsumePreparedChart(game* gameState, sqlite3* songDatabase)
{
	return g_client && g_client->ConsumePreparedChart(gameState, songDatabase);
}

void ApplyPlaySettings(game* gameState)
{
	if (g_client) g_client->ApplyPlaySettings(gameState);
}

bool WaitForSynchronizedStart(game* gameState)
{
	return !g_client || g_client->WaitForStart(gameState);
}

bool IsArenaPlayActive()
{
	return g_client && g_client->Active();
}

bool BlocksAbortInput()
{
	return IsArenaPlayActive();
}

bool BlocksQuickRestart()
{
	return g_client && g_client->ArenaChart();
}

bool IgnorePlayStartInputDelay()
{
	return IsArenaPlayActive();
}

} // namespace openlr2::arena
