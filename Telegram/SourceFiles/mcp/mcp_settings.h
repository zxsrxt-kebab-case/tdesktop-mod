/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"

namespace Mcp {

// What an MCP client is allowed to do. Each one hides its tools from the
// list entirely when off, so a client never even learns they exist.
enum class Capability : uint32 {
	Read   = (1U << 0), // Chats, topics, history, search, saved versions.
	Drafts = (1U << 1), // Put text into the compose field, never sends.
	Send   = (1U << 2), // Send messages and read receipts as the user.
	Manage = (1U << 3), // Edit and delete messages.
	Events = (1U << 4), // Subscriptions and the pushed event stream.
	Client = (1U << 5), // Toggle this client's own settings.
};
inline constexpr bool is_flag_type(Capability) { return true; }
using Capabilities = base::flags<Capability>;

inline constexpr auto kDefaultPort = 47821;
inline constexpr auto kMinPort = 1024;
inline constexpr auto kMaxPort = 65535;

// Kept in the generic prefs of Core::Settings rather than in its binary
// stream: none of it is needed before the settings are read, and the prefs
// grow without versioning the stream.
class Settings final {
public:
	Settings();

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);

	[[nodiscard]] int port() const;
	[[nodiscard]] rpl::producer<int> portValue() const;
	void setPort(int port);

	[[nodiscard]] QByteArray token() const;
	[[nodiscard]] rpl::producer<QByteArray> tokenValue() const;
	void regenerateToken();

	[[nodiscard]] Capabilities capabilities() const;
	[[nodiscard]] rpl::producer<Capabilities> capabilitiesValue() const;
	[[nodiscard]] bool has(Capability capability) const;
	void setCapability(Capability capability, bool enabled);

private:
	void save();

	rpl::variable<bool> _enabled = false;
	rpl::variable<int> _port = kDefaultPort;
	rpl::variable<QByteArray> _token;
	rpl::variable<Capabilities> _capabilities;

};

} // namespace Mcp
