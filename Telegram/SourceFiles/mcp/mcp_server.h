/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

class QTcpServer;
class QTcpSocket;

namespace Mcp {

// A small HTTP/1.1 listener for the MCP "Streamable HTTP" transport: JSON-RPC
// in POST bodies, server-initiated messages over a long-lived GET event
// stream.
//
// It only ever binds loopback, every request has to carry the token, and
// anything only a browser would send is refused - an Origin header, or a
// Host that is not this machine - so a web page cannot reach it, not even
// through DNS rebinding. Everything runs on the main thread, which is also
// the only thread the session data may be touched from.
class Server final : public base::has_weak_ptr {
public:
	using Done = Fn<void(QJsonValue response)>;
	using Handler = Fn<void(QJsonValue message, Done done)>;

	explicit Server(Handler handler);
	~Server();

	[[nodiscard]] bool listen(int port, QByteArray token, QString *error);
	void stop();

	// Sent to every open event stream as one Server-Sent Event.
	void broadcast(const QJsonObject &message);
	[[nodiscard]] rpl::producer<int> streamsValue() const;

private:
	struct Request;
	struct Connection;

	void accept();
	void read(uint64 id);
	void handle(uint64 id, Request &&request);
	void reply(
		uint64 id,
		int status,
		QByteArray contentType,
		QByteArray body,
		QByteArray extraHeaders = QByteArray());
	void openStream(uint64 id);
	void close(uint64 id);
	void sendKeepAlive();
	void refreshStreamsCount();

	[[nodiscard]] Connection *find(uint64 id) const;
	[[nodiscard]] bool hostAllowed(const QByteArray &host) const;

	const Handler _handler;
	std::unique_ptr<QTcpServer> _tcp;
	QByteArray _token;
	int _port = 0;
	base::flat_map<uint64, std::unique_ptr<Connection>> _connections;
	uint64 _lastConnectionId = 0;
	base::Timer _keepAlive;
	rpl::variable<int> _streams = 0;

};

} // namespace Mcp
