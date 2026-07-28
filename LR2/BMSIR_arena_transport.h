#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openlr2::arena {

class WebSocketTransport {
public:
	WebSocketTransport();
	~WebSocketTransport();
	WebSocketTransport(const WebSocketTransport&) = delete;
	WebSocketTransport& operator=(const WebSocketTransport&) = delete;

	void Start(std::string url);
	void Stop();
	bool Send(std::string_view message);
	[[nodiscard]] bool Connected() const;
	[[nodiscard]] bool Running() const;
	[[nodiscard]] std::string LastError() const;
	std::vector<std::string> DrainMessages();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace openlr2::arena
