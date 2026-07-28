#include "BMSIR_arena.h"

#include "BMSIR_arena_log.h"
#include "BMSIR_arena_protocol.h"
#include "BMSIR_arena_transport.h"
#include "LR2.h"
#include "LR2_songmanage.h"
#include "LR2_version.h"
#include "Scene02_Songselect.h"
#include "structure.h"

#include <DxLib.h>
#include <nlohmann/json.hpp>

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
#include <thread>

#ifndef OPENLR2_ARENA_BUILD_HASH
#define OPENLR2_ARENA_BUILD_HASH "unknown"
#endif

namespace openlr2::arena {
namespace {

using Clock = std::chrono::steady_clock;

struct ArenaConfig {
	bool enabled{};
	bool showOverlay{true};
	bool allowCpu{true};
	bool unrestrictedRating{};
	int playerId{};
	std::string server{"wss://www.bms-ir.org/new/arena/ws/client"};
};

class Client {
public:
	explicit Client(game* gameState)
		: game_(gameState), config_(ReadConfig())
	{
		LogEvent("initialize", {
			{"enabled", config_.enabled},
			{"client_version", std::string(kClientVersion)},
			{"body_version", openlr2::versionName},
			{"client_flavor", std::string(kClientFlavor)},
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
		if (!config_.enabled || shutdown_) return;
		HandleTransportState();
		for (auto& raw : transport_.DrainMessages()) {
			try {
				auto message = nlohmann::json::parse(raw);
				if (!message.is_object()) throw std::runtime_error("object required");
				LogMessage("inbound", message);
				HandleMessage(message, database);
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
		HandleHotkey();
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
		if (!config_.enabled || !config_.showOverlay) return;
		const int color = authenticated_ ? GetColor(180, 255, 180) : GetColor(255, 210, 120);
		const int background = GetColor(12, 12, 18);
		SetDrawBlendMode(DX_BLENDMODE_ALPHA, 210);
		DrawBox(8, 8, 620, reserved_ ? 92 : 70, background, TRUE);
		SetDrawBlendMode(DX_BLENDMODE_NOBLEND, 255);
		DrawString(16, 14, "BMS-IR Arena for OpenLR2", color);
		const char* insertAction = phase_ == "option lock"
			? "READY option"
			: "enter/leave";
		DrawFormatString(
			16,
			34,
			GetColor(240, 240, 240),
			"Insert: %s  status: %s",
			insertAction,
			status_.c_str());
		if (reserved_) {
			DrawFormatString(
				16,
				54,
				GetColor(190, 220, 255),
				"phase: %s  match: %.12s  chart: %.32s",
				phase_.c_str(),
				matchId_.c_str(),
				chartHash_.c_str());
			DrawString(16, 74, "diagnostics: bmsir-arena.log", GetColor(160, 160, 170));
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
		const bool doublePlay = playMode_ == 10 || playMode_ == 14 || playMode_ == 50;
		play.random[PLAYER_1] = NormalizeArenaRandom(playOption_ % 10);
		play.random[PLAYER_2] = doublePlay
			? NormalizeArenaRandom((playOption_ / 10) % 10)
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
			if (start > 0.0 && EpochSeconds() + serverOffset_.load() >= start) {
				LogEvent("synchronized_start_released", {
					{"match_id", matchId},
					{"chart_hash", chartHash},
				});
				return true;
			}
			const double deadline = loadDeadline_.load();
			if (deadline > 0.0
				&& EpochSeconds() + serverOffset_.load() > deadline + 5.0) {
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
			result.playerId = value.value("player_id", 0);
			result.server = value.value(
				"server",
				std::string("wss://www.bms-ir.org/new/arena/ws/client"));
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

	static double EpochSeconds()
	{
		return std::chrono::duration<double>(
			std::chrono::system_clock::now().time_since_epoch()).count();
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
				{"client_version", kClientVersion},
				{"body_version", openlr2::versionName},
				{"build_hash", OPENLR2_ARENA_BUILD_HASH},
				{"arena_enabled", true},
				{"client_flavor", kClientFlavor},
				{"ruleset_profile", kRulesetProfile},
			};
			helloSent_ = transport_.Send(hello.dump());
			if (helloSent_) {
				LogEvent("hello_sent", {
					{"player_id", config_.playerId},
					{"client_version", std::string(kClientVersion)},
					{"client_flavor", std::string(kClientFlavor)},
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

	void HandleMessage(const nlohmann::json& message, sqlite3* database)
	{
		const std::string type = message.value("type", "");
		if (type == "hello_ok") {
			if (message.value("ruleset_profile", "") != kRulesetProfile) {
				status_ = "server ruleset mismatch";
				LogEvent("hello_ruleset_mismatch", {
					{"ruleset_profile", message.value("ruleset_profile", "")},
				});
				transport_.Stop();
				return;
			}
			authenticated_ = true;
			status_ = "connected; Insert to enter";
			UpdateClock(message);
			SendState();
			return;
		}
		if (type == "pong") {
			UpdateClock(message);
			return;
		}
		if (type == "arena_status") {
			if (const auto player = message.find("player");
				player != message.end() && player->is_object()) {
				if (const auto queue = player->find("queue");
					queue != player->end() && queue->is_object()) {
					const std::string previousQueueStatus = queueStatus_;
					queueStatus_ = queue->value("status", "idle");
					reserved_ = queueStatus_ == "reserved"
						|| queueStatus_ == "matched"
						|| queueStatus_ == "withdraw_requested";
					if (queueStatus_ == "queued") status_ = "waiting for opponent";
					else if (queueStatus_ == "cancelled" || queueStatus_ == "idle") {
						status_ = "connected; Insert to enter";
					}
					if (queueStatus_ != previousQueueStatus) {
						LogEvent("queue_status_changed", {
							{"from", previousQueueStatus},
							{"to", queueStatus_},
							{"match_id", matchId_},
						});
					}
				}
			}
			return;
		}
		if (type == "fill_started") {
			phase_ = "fill";
			status_ = "match filling";
			return;
		}
		if (type == "players_updated") return;
		if (type == "match_reserved") {
			if (message.value("ruleset_profile", "") != kRulesetProfile) {
				status_ = "match ruleset mismatch";
				LogEvent("match_ruleset_mismatch", {
					{"match_id", message.value("match_id", "")},
					{"ruleset_profile", message.value("ruleset_profile", "")},
				});
				return;
			}
			matchId_ = message.value("match_id", "");
			reserved_ = true;
			queueStatus_ = "reserved";
			phase_ = "nomination";
			status_ = "matched; waiting for nomination";
			if (const auto rules = message.find("rules");
				rules != message.end() && rules->is_object()) {
				forcedGauge_ = rules->value("forced_gauge", "free");
			}
			ResetMatchTransient();
			LogEvent("match_reserved", {
				{"match_id", matchId_},
				{"ruleset_profile", std::string(kRulesetProfile)},
				{"forced_gauge", forcedGauge_},
			});
			return;
		}
		if (type == "nomination_started" || type == "nomination_status") {
			phase_ = "nomination";
			if (!nominationSkipped_ && !matchId_.empty()) {
				nominationSkipped_ = true;
				Send(MatchMessage("chart_nomination_skip"));
				status_ = "server-random nomination submitted";
			}
			return;
		}
		if (type == "nominations_revealed") {
			phase_ = "chart check";
			return;
		}
		if (type == "chart") {
			ReceiveChart(message, database);
			return;
		}
		if (type == "option_select") {
			phase_ = "option lock";
			if (!optionReadySent_ && chartAvailable_) {
				status_ = "choose lane option; Insert to READY";
			}
			return;
		}
		if (type == "prepare") {
			phase_ = "loading";
			randomSeed_ = message.value("random_seed", randomSeed_);
			playOption_ = message.value("play_option", playOption_);
			playMode_ = message.value("play_mode", playMode_);
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
			const std::string remoteState = message.value("state", "");
			if (!remoteState.empty()) phase_ = remoteState;
			return;
		}
		if (type == "result") {
			status_ = "Arena result received";
			phase_ = "result";
			LogEvent("result_received", {
				{"match_id", matchId_},
				{"players", message.value("players", nlohmann::json::array()).size()},
			});
			EndMatch();
			return;
		}
		if (type == "match_cancelled" || type == "match_released"
			|| type == "forfeit_accepted") {
			status_ = message.value("reason", type);
			EndMatch();
			return;
		}
		if (type == "match_resume") {
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

	void ReceiveChart(const nlohmann::json& message, sqlite3* database)
	{
		const auto chart = message.value("chart", nlohmann::json::object());
		chartHash_ = chart.value("md5", "");
		chartTotalNotes_ = chart.value("totalnotes", 0);
		randomSeed_ = message.value("random_seed", 0L);
		SONGDATA song{};
		InitSongData(&song);
		chartAvailable_ = IsMd5(chartHash_)
			&& GetSongData(CSTR(chartHash_.c_str()), &song, database, &game_->sSelect) == 1;
		playMode_ = chartAvailable_ ? song.keymode : 0;
		Send(MatchMessage("chart_check", {
			{"chart_hash", chartHash_},
			{"available", chartAvailable_},
			// Existing OpenLR2 song.db note totals may predate Arena LN
			// normalization. Zero explicitly means "owned, total unknown".
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
	}

	void HandleHotkey()
	{
		if (!authenticated_ || game_->procSelecter != 2
			|| game_->KeyInput.inputID[KEY_INPUT_INSERT] != 1) return;
		if (reserved_ && phase_ == "option lock" && chartAvailable_
			&& !optionReadySent_) {
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
			return;
		}
		if (active_) return;
		if (queueStatus_ == "idle" || queueStatus_.empty()
			|| queueStatus_ == "cancelled") {
			Send({
				{"type", "queue_entry"},
				{"unrestricted_rating", config_.unrestrictedRating},
				{"allow_cpu", config_.allowCpu},
			});
			status_ = "entry requested";
		}
		else {
			Send({{"type", "queue_cancel"}});
			status_ = "leave requested";
		}
	}

	void SendLiveIfDue()
	{
		if (!active_ || scene_ != "play" || startAt_ <= 0.0
			|| EpochSeconds() + serverOffset_ < startAt_) return;
		const auto now = Clock::now();
		if (now - lastLive_ < std::chrono::seconds(1)) return;
		lastLive_ = now;
		const auto& player = game_->gameplay.player[PLAYER_1];
		const int totalNotes = chartTotalNotes_ > 0
			? chartTotalNotes_
			: std::max(player.totalnotes, 0);
		const int processed = std::clamp(
			player.note_current,
			0,
			totalNotes);
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
		const bool ready = reserved_ && !active_
			&& (scene_ == "select" || scene_ == "result");
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
			{"type", type},
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
		sequence_ = 0;
		startAt_ = 0.0;
		loadDeadline_ = 0.0;
		randomSeed_ = 0;
		chartHash_.clear();
		chartTotalNotes_ = 0;
		playMode_ = 0;
		playOption_ = 0;
	}

	void EndMatch()
	{
		active_ = false;
		reserved_ = false;
		queueStatus_ = "idle";
		launchRequested_ = false;
		startAt_ = 0.0;
		loadDeadline_ = 0.0;
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

	game* game_{};
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
	bool arenaChart_{};
	std::atomic_bool active_{false};
	std::atomic<double> startAt_{0.0};
	std::atomic<double> loadDeadline_{0.0};
	std::atomic<double> serverOffset_{0.0};
	std::string scene_{"unknown"};
	std::string status_{"disabled"};
	std::string phase_{"idle"};
	std::string queueStatus_{"idle"};
	std::string matchId_;
	std::string chartHash_;
	std::string forcedGauge_{"free"};
	std::string lastTransportError_;
	int chartTotalNotes_{};
	int playMode_{};
	int playOption_{};
	long long randomSeed_{};
	long long sequence_{};
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
