#include "BMSIR_arena_transport.h"

#include <array>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace openlr2::arena {

struct WebSocketTransport::Impl {
	std::atomic_bool connected{false};
	std::atomic_bool running{false};
	mutable std::mutex mutex;
	std::queue<std::string> messages;
	std::string error;
	std::string url;
	std::jthread worker;
#ifdef _WIN32
	HINTERNET session{};
	HINTERNET connection{};
	HINTERNET request{};
	HINTERNET socket{};
#endif

	void SetError(std::string value)
	{
		std::scoped_lock lock(mutex);
		error = std::move(value);
	}

#ifdef _WIN32
	static std::string WindowsError(const char* operation, const DWORD code)
	{
		return std::string(operation) + " failed (" + std::to_string(code) + ")";
	}

	void CloseHandles()
	{
		std::scoped_lock lock(mutex);
		if (socket) {
			WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
			WinHttpCloseHandle(socket);
			socket = nullptr;
		}
		if (request) {
			WinHttpCloseHandle(request);
			request = nullptr;
		}
		if (connection) {
			WinHttpCloseHandle(connection);
			connection = nullptr;
		}
		if (session) {
			WinHttpCloseHandle(session);
			session = nullptr;
		}
	}

	void Run(const std::stop_token stop)
	{
		running = true;
		HINTERNET sessionHandle{};
		HINTERNET connectionHandle{};
		HINTERNET requestHandle{};
		HINTERNET socketHandle{};
		std::wstring wideUrl(url.begin(), url.end());
		const bool secure = wideUrl.starts_with(L"wss://");
		if (secure) {
			wideUrl.replace(0, 6, L"https://");
		}
		else if (wideUrl.starts_with(L"ws://")) {
			wideUrl.replace(0, 5, L"http://");
		}
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = static_cast<DWORD>(-1);
		parts.dwHostNameLength = static_cast<DWORD>(-1);
		parts.dwUrlPathLength = static_cast<DWORD>(-1);
		parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
			SetError(WindowsError("WinHttpCrackUrl", GetLastError()));
			running = false;
			return;
		}
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
		if (parts.dwExtraInfoLength > 0) {
			path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		}
		if (path.empty()) path = L"/";

		{
			std::scoped_lock lock(mutex);
			session = WinHttpOpen(
				L"OpenLR2-BMSIR-Arena/0.2",
				WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
				WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS,
				0);
			if (session) connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
			if (connection) {
				request = WinHttpOpenRequest(
					connection,
					L"GET",
					path.c_str(),
					nullptr,
					WINHTTP_NO_REFERER,
					WINHTTP_DEFAULT_ACCEPT_TYPES,
					secure ? WINHTTP_FLAG_SECURE : 0);
			}
			sessionHandle = session;
			connectionHandle = connection;
			requestHandle = request;
		}
		if (!sessionHandle || !connectionHandle || !requestHandle) {
			SetError(WindowsError("WinHTTP setup", GetLastError()));
			CloseHandles();
			running = false;
			return;
		}
		if (!WinHttpSetOption(requestHandle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
			|| !WinHttpSendRequest(
				requestHandle,
				WINHTTP_NO_ADDITIONAL_HEADERS,
				0,
				WINHTTP_NO_REQUEST_DATA,
				0,
				0,
				0)
			|| !WinHttpReceiveResponse(requestHandle, nullptr)) {
			SetError(WindowsError("WebSocket upgrade", GetLastError()));
			CloseHandles();
			running = false;
			return;
		}
		{
			std::scoped_lock lock(mutex);
			socketHandle = WinHttpWebSocketCompleteUpgrade(requestHandle, 0);
			if (request == requestHandle) {
				WinHttpCloseHandle(request);
				request = nullptr;
			}
			socket = socketHandle;
		}
		if (!socketHandle) {
			SetError(WindowsError("WinHttpWebSocketCompleteUpgrade", GetLastError()));
			CloseHandles();
			running = false;
			return;
		}

		connected = true;
		std::string message;
		std::array<char, 64 * 1024> buffer{};
		while (!stop.stop_requested()) {
			DWORD read = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
			const DWORD result = WinHttpWebSocketReceive(
				socketHandle,
				buffer.data(),
				static_cast<DWORD>(buffer.size()),
				&read,
				&type);
			if (result != ERROR_SUCCESS) {
				if (!stop.stop_requested()) SetError(WindowsError("WebSocket receive", result));
				break;
			}
			if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
			if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE
				&& type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
				SetError("Arena server sent a non-text WebSocket frame");
				break;
			}
			if (message.size() + read > 256 * 1024) {
				SetError("Arena WebSocket message exceeded 256 KiB");
				break;
			}
			message.append(buffer.data(), read);
			if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
				std::scoped_lock lock(mutex);
				messages.push(std::move(message));
				message.clear();
			}
		}
		connected = false;
		CloseHandles();
		running = false;
	}
#else
	void Run(const std::stop_token)
	{
		SetError("BMS-IR Arena transport is available only in Windows builds");
		running = false;
	}
#endif
};

WebSocketTransport::WebSocketTransport()
	: impl_(std::make_unique<Impl>())
{
}

WebSocketTransport::~WebSocketTransport()
{
	Stop();
}

void WebSocketTransport::Start(std::string url)
{
	Stop();
	impl_->url = std::move(url);
	{
		std::scoped_lock lock(impl_->mutex);
		impl_->error.clear();
	}
	impl_->worker = std::jthread([this](const std::stop_token stop) {
		impl_->Run(stop);
	});
}

void WebSocketTransport::Stop()
{
	if (!impl_) return;
	if (impl_->worker.joinable()) {
		impl_->worker.request_stop();
#ifdef _WIN32
		impl_->CloseHandles();
#endif
		impl_->worker.join();
	}
	impl_->connected = false;
	impl_->running = false;
}

bool WebSocketTransport::Send(const std::string_view message)
{
#ifdef _WIN32
	std::scoped_lock lock(impl_->mutex);
	if (!impl_->connected || !impl_->socket) return false;
	const DWORD result = WinHttpWebSocketSend(
		impl_->socket,
		WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
		const_cast<char*>(message.data()),
		static_cast<DWORD>(message.size()));
	if (result == ERROR_SUCCESS) return true;
	impl_->error = Impl::WindowsError("WebSocket send", result);
	return false;
#else
	(void)message;
	return false;
#endif
}

bool WebSocketTransport::Connected() const
{
	return impl_->connected;
}

bool WebSocketTransport::Running() const
{
	return impl_->running;
}

std::string WebSocketTransport::LastError() const
{
	std::scoped_lock lock(impl_->mutex);
	return impl_->error;
}

std::vector<std::string> WebSocketTransport::DrainMessages()
{
	std::vector<std::string> result;
	std::scoped_lock lock(impl_->mutex);
	while (!impl_->messages.empty()) {
		result.push_back(std::move(impl_->messages.front()));
		impl_->messages.pop();
	}
	return result;
}

} // namespace openlr2::arena
