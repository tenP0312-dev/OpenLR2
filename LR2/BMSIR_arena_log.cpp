#include "BMSIR_arena_log.h"

#include "BMSIR_arena_protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>

namespace openlr2::arena {
namespace {

constexpr std::uintmax_t kMaxBytes = 2 * 1024 * 1024;
constexpr int kMaxBackups = 5;
const std::filesystem::path kLogPath = "bmsir-arena.log";
std::mutex g_logMutex;

std::string Timestamp()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t value = std::chrono::system_clock::to_time_t(now);
	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &value);
#else
	gmtime_r(&value, &utc);
#endif
	std::ostringstream result;
	result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return result.str();
}

bool SensitiveKey(const std::string& key)
{
	std::string normalized;
	normalized.reserve(key.size());
	std::ranges::transform(key, std::back_inserter(normalized), [](const unsigned char item) {
		return static_cast<char>(std::tolower(item));
	});
	return normalized.find("pass") != std::string::npos
		|| normalized.find("token") != std::string::npos
		|| normalized.find("fingerprint") != std::string::npos
		|| normalized.find("build_hash") != std::string::npos
		|| normalized.find("plugin_hash") != std::string::npos
		|| normalized == "text";
}

void Rotate()
{
	std::error_code error;
	if (!std::filesystem::exists(kLogPath, error)
		|| std::filesystem::file_size(kLogPath, error) < kMaxBytes) {
		return;
	}
	for (int index = kMaxBackups - 1; index >= 1; --index) {
		const auto source = kLogPath.string() + "." + std::to_string(index);
		const auto target = kLogPath.string() + "." + std::to_string(index + 1);
		if (std::filesystem::exists(source, error)) {
			std::filesystem::rename(source, target, error);
			error.clear();
		}
	}
	std::filesystem::rename(kLogPath, kLogPath.string() + ".1", error);
}

} // namespace

void LogEvent(const std::string_view event)
{
	LogEvent(event, nlohmann::json::object());
}

void LogEvent(const std::string_view event, nlohmann::json details)
{
	try {
		std::scoped_lock lock(g_logMutex);
		Rotate();
		nlohmann::json line = {
			{"at", Timestamp()},
			{"event", std::string(event).substr(0, 80)},
		};
		if (details.is_object()) {
			for (auto& [key, value] : details.items()) {
				if (SensitiveKey(key)) continue;
				if (value.is_string()) {
					line[key.substr(0, 80)] = value.get<std::string>().substr(0, 500);
				}
				else if (value.is_boolean() || value.is_number() || value.is_null()) {
					line[key.substr(0, 80)] = value;
				}
			}
		}
		std::ofstream stream(kLogPath, std::ios::binary | std::ios::app);
		stream << line.dump() << '\n';
	}
	catch (...) {
		// Diagnostics must never interrupt rendering, input, or networking.
	}
}

void LogMessage(
	const std::string_view direction,
	const nlohmann::json& message)
{
	auto details = DiagnosticMessage(direction, message);
	const std::string event = details.value("event", "message");
	details.erase("event");
	LogEvent(event, std::move(details));
}

} // namespace openlr2::arena
