/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "mcp/mcp_events.h"
#include "mcp/mcp_settings.h"
#include "mcp/mcp_tools.h"

namespace Mcp {

class Server;

enum class State {
	Off,
	Running,
	Locked, // Stopped while the local passcode lock is on.
	Failed,
};

struct Status {
	State state = State::Off;
	int port = 0;
	int streams = 0;
	QString error;

	friend inline bool operator==(const Status &, const Status &) = default;
};

// The MCP server of this client: owns its settings, runs the listener
// whenever it is turned on and the app is not locked, and answers the
// JSON-RPC that comes in.
class Manager final : public base::has_weak_ptr {
public:
	Manager();
	~Manager();

	[[nodiscard]] Settings &settings();
	[[nodiscard]] Status status() const;
	[[nodiscard]] rpl::producer<Status> statusValue() const;
	[[nodiscard]] QString address() const;

private:
	using Done = Fn<void(QJsonValue)>;

	void refresh();
	void start();
	void stop();

	void handle(QJsonValue message, Done done);
	void handleOne(const QJsonValue &message, Done done);
	[[nodiscard]] QJsonObject initialize(const QJsonObject &params) const;
	void push(const QJsonObject &event);
	void notify(const QString &method, QJsonObject params = {});

	Settings _settings;
	Events _events;
	Tools _tools;
	std::unique_ptr<Server> _server;
	rpl::variable<Status> _status;
	QByteArray _runningToken;
	rpl::lifetime _serverLifetime;
	rpl::lifetime _lifetime;

};

} // namespace Mcp
