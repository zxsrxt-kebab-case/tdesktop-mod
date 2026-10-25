/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mcp/mcp_server.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace Mcp {
namespace {

constexpr auto kMaxConnections = 32;
constexpr auto kMaxHeaderSize = 64 * 1024;
constexpr auto kMaxBodySize = 8 * 1024 * 1024;
constexpr auto kKeepAliveInterval = crl::time(15000);
constexpr auto kIdleTimeout = crl::time(120000);

enum class ParseResult {
	Incomplete,
	Done,
	Bad,
	TooLarge,
};

[[nodiscard]] QByteArray StatusText(int status) {
	switch (status) {
	case 200: return "OK";
	case 202: return "Accepted";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 413: return "Payload Too Large";
	case 503: return "Service Unavailable";
	}
	return "Unknown";
}

// Compares the whole secret every time, so the time taken does not tell how
// much of a guess was right.
[[nodiscard]] bool SameSecret(const QByteArray &a, const QByteArray &b) {
	if (a.size() != b.size() || a.isEmpty()) {
		return false;
	}
	auto difference = 0;
	for (auto i = 0; i != a.size(); ++i) {
		difference |= (a[i] ^ b[i]);
	}
	return (difference == 0);
}

[[nodiscard]] QByteArray ErrorBody(int code, const QByteArray &message) {
	return QJsonDocument(QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, QJsonValue() },
		{ u"error"_q, QJsonObject{
			{ u"code"_q, code },
			{ u"message"_q, QString::fromUtf8(message) },
		} },
	}).toJson(QJsonDocument::Compact);
}

} // namespace

struct Server::Request {
	QByteArray method;
	QByteArray path;
	base::flat_map<QByteArray, QByteArray> headers;
	QByteArray body;

	[[nodiscard]] QByteArray header(const QByteArray &name) const {
		const auto i = headers.find(name);
		return (i != headers.end()) ? i->second : QByteArray();
	}
};

struct Server::Connection {
	QPointer<QTcpSocket> socket;
	QByteArray buffer;
	crl::time lastActivity = 0;
	bool busy = false;
	bool stream = false;
	bool closeAfter = false;
};

namespace {

// Reads one request off the front of the buffer, removing it from there.
// Bodies come either with a Content-Length or chunked; nothing else is
// accepted, which is fine for the one kind of client this talks to.
[[nodiscard]] ParseResult ParseRequest(
		QByteArray &buffer,
		QByteArray &method,
		QByteArray &path,
		base::flat_map<QByteArray, QByteArray> &headers,
		QByteArray &body) {
	const auto headerEnd = buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		return (buffer.size() > kMaxHeaderSize)
			? ParseResult::TooLarge
			: ParseResult::Incomplete;
	}
	const auto lines = buffer.left(headerEnd).split('\n');
	const auto requestLine = lines.front().trimmed().split(' ');
	if (requestLine.size() < 3) {
		return ParseResult::Bad;
	}
	method = requestLine[0].toUpper();
	path = requestLine[1];
	headers.clear();
	for (auto i = 1; i < lines.size(); ++i) {
		const auto line = lines[i].trimmed();
		if (line.isEmpty()) {
			continue;
		}
		const auto colon = line.indexOf(':');
		if (colon <= 0) {
			return ParseResult::Bad;
		}
		headers[line.left(colon).trimmed().toLower()]
			= line.mid(colon + 1).trimmed();
	}
	const auto bodyStart = headerEnd + 4;
	const auto encoding = headers.find(QByteArray("transfer-encoding"));
	if (encoding != headers.end()
		&& encoding->second.toLower().contains("chunked")) {
		auto result = QByteArray();
		auto position = qsizetype(bodyStart);
		while (true) {
			const auto lineEnd = buffer.indexOf("\r\n", position);
			if (lineEnd < 0) {
				return ParseResult::Incomplete;
			}
			auto sizeLine = buffer.mid(position, lineEnd - position);
			const auto semicolon = sizeLine.indexOf(';');
			if (semicolon >= 0) {
				sizeLine = sizeLine.left(semicolon);
			}
			auto ok = false;
			const auto size = sizeLine.trimmed().toLongLong(&ok, 16);
			if (!ok || size < 0) {
				return ParseResult::Bad;
			} else if (result.size() + size > kMaxBodySize) {
				return ParseResult::TooLarge;
			} else if (!size) {
				const auto end = buffer.indexOf("\r\n\r\n", lineEnd);
				if (end < 0) {
					return ParseResult::Incomplete;
				}
				body = std::move(result);
				buffer.remove(0, end + 4);
				return ParseResult::Done;
			}
			const auto dataStart = lineEnd + 2;
			if (buffer.size() < dataStart + size + 2) {
				return ParseResult::Incomplete;
			}
			result.append(buffer.mid(dataStart, size));
			position = dataStart + size + 2;
		}
	}
	auto length = qint64(0);
	const auto contentLength = headers.find(QByteArray("content-length"));
	if (contentLength != headers.end()) {
		auto ok = false;
		length = contentLength->second.toLongLong(&ok);
		if (!ok || length < 0) {
			return ParseResult::Bad;
		}
	}
	if (length > kMaxBodySize) {
		return ParseResult::TooLarge;
	} else if (buffer.size() < bodyStart + length) {
		return ParseResult::Incomplete;
	}
	body = buffer.mid(bodyStart, length);
	buffer.remove(0, bodyStart + length);
	return ParseResult::Done;
}

} // namespace

Server::Server(Handler handler)
: _handler(std::move(handler))
, _keepAlive([=] { sendKeepAlive(); }) {
}

Server::~Server() {
	stop();
}

bool Server::listen(int port, QByteArray token, QString *error) {
	stop();

	_token = std::move(token);
	_port = port;
	_tcp = std::make_unique<QTcpServer>();
	_tcp->setMaxPendingConnections(kMaxConnections);
	if (!_tcp->listen(QHostAddress::LocalHost, quint16(port))) {
		if (error) {
			*error = _tcp->errorString();
		}
		_tcp = nullptr;
		return false;
	}
	QObject::connect(_tcp.get(), &QTcpServer::newConnection, [=] {
		accept();
	});
	_keepAlive.callEach(kKeepAliveInterval);
	return true;
}

void Server::stop() {
	_keepAlive.cancel();
	for (const auto &[id, connection] : base::take(_connections)) {
		if (const auto socket = connection->socket.data()) {
			socket->disconnect();
			socket->abort();
			socket->deleteLater();
		}
	}
	if (_tcp) {
		_tcp->close();
		_tcp = nullptr;
	}
	refreshStreamsCount();
}

void Server::accept() {
	while (const auto socket = _tcp->nextPendingConnection()) {
		if (_connections.size() >= kMaxConnections) {
			socket->abort();
			socket->deleteLater();
			continue;
		}
		const auto id = ++_lastConnectionId;
		auto connection = std::make_unique<Connection>();
		connection->socket = socket;
		connection->lastActivity = crl::now();
		_connections.emplace(id, std::move(connection));

		QObject::connect(socket, &QTcpSocket::readyRead, socket, [=] {
			read(id);
		});
		QObject::connect(socket, &QTcpSocket::disconnected, socket, [=] {
			close(id);
		});
	}
}

Server::Connection *Server::find(uint64 id) const {
	const auto i = _connections.find(id);
	return (i != _connections.end()) ? i->second.get() : nullptr;
}

void Server::read(uint64 id) {
	const auto connection = find(id);
	if (!connection || !connection->socket) {
		return;
	}
	connection->buffer.append(connection->socket->readAll());
	connection->lastActivity = crl::now();
	if (connection->stream) {
		// Nothing is expected from the client on an event stream.
		connection->buffer.clear();
		return;
	} else if (connection->busy) {
		// The request in flight is answered first; whatever came in after
		// it waits in the buffer until then.
		return;
	}
	auto request = Request();
	const auto result = ParseRequest(
		connection->buffer,
		request.method,
		request.path,
		request.headers,
		request.body);
	switch (result) {
	case ParseResult::Incomplete:
		return;
	case ParseResult::Bad:
		connection->closeAfter = true;
		reply(id, 400, "text/plain", "Bad request");
		return;
	case ParseResult::TooLarge:
		connection->closeAfter = true;
		reply(id, 413, "text/plain", "Too large");
		return;
	case ParseResult::Done:
		handle(id, std::move(request));
		return;
	}
}

bool Server::hostAllowed(const QByteArray &host) const {
	const auto lower = host.toLower();
	const auto port = QByteArray::number(_port);
	for (const auto &name : {
			QByteArray("127.0.0.1"),
			QByteArray("localhost"),
			QByteArray("[::1]") }) {
		if (lower == name || lower == (name + ':' + port)) {
			return true;
		}
	}
	return false;
}

void Server::handle(uint64 id, Request &&request) {
	const auto connection = find(id);
	if (!connection) {
		return;
	}
	if (request.header("connection").toLower() == "close") {
		connection->closeAfter = true;
	}

	auto path = request.path;
	if (path.startsWith("http://")) {
		const auto slash = path.indexOf('/', 7);
		path = (slash >= 0) ? path.mid(slash) : QByteArray("/");
	}
	if (const auto query = path.indexOf('?'); query >= 0) {
		path = path.left(query);
	}

	if (!hostAllowed(request.header("host"))) {
		connection->closeAfter = true;
		reply(id, 403, "text/plain", "Forbidden host");
		return;
	} else if (!request.header("origin").isEmpty()) {
		// Every browser request carries an Origin and no MCP client does.
		connection->closeAfter = true;
		reply(id, 403, "text/plain", "Browsers are not allowed");
		return;
	} else if (path != "/mcp" && path != "/mcp/") {
		reply(id, 404, "text/plain", "Not found");
		return;
	}
	const auto authorization = request.header("authorization");
	const auto bearer = QByteArray("bearer ");
	const auto token = authorization.left(bearer.size()).toLower() == bearer
		? authorization.mid(bearer.size()).trimmed()
		: QByteArray();
	if (!SameSecret(token, _token)) {
		reply(
			id,
			401,
			"text/plain",
			"Unauthorized",
			"WWW-Authenticate: Bearer realm=\"kebabclient-mcp\"\r\n");
		return;
	}

	if (request.method == "GET") {
		if (request.header("accept").toLower().contains("text/event-stream")) {
			openStream(id);
		} else {
			reply(
				id,
				405,
				"text/plain",
				"Use POST",
				"Allow: GET, POST, DELETE\r\n");
		}
		return;
	} else if (request.method == "DELETE") {
		// Nothing is kept per session, so there is nothing to end.
		reply(id, 200, "text/plain", QByteArray());
		return;
	} else if (request.method != "POST") {
		reply(
			id,
			405,
			"text/plain",
			"Method not allowed",
			"Allow: GET, POST, DELETE\r\n");
		return;
	}

	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(request.body, &error);
	if (error.error != QJsonParseError::NoError
		|| (!document.isObject() && !document.isArray())) {
		reply(id, 400, "application/json", ErrorBody(-32700, "Parse error"));
		return;
	}
	const auto message = document.isObject()
		? QJsonValue(document.object())
		: QJsonValue(document.array());

	connection->busy = true;
	_handler(message, crl::guard(this, [=](QJsonValue response) {
		const auto connection = find(id);
		if (!connection) {
			return;
		}
		connection->busy = false;
		if (response.isNull() || response.isUndefined()) {
			reply(id, 202, "text/plain", QByteArray());
		} else {
			const auto body = response.isArray()
				? QJsonDocument(response.toArray())
				: QJsonDocument(response.toObject());
			reply(
				id,
				200,
				"application/json; charset=utf-8",
				body.toJson(QJsonDocument::Compact));
		}
		// Whatever was pipelined behind this request is handled now, from
		// a clean stack.
		crl::on_main(this, [=] {
			read(id);
		});
	}));
}

void Server::reply(
		uint64 id,
		int status,
		QByteArray contentType,
		QByteArray body,
		QByteArray extraHeaders) {
	const auto connection = find(id);
	if (!connection || !connection->socket) {
		return;
	}
	const auto socket = connection->socket.data();
	auto response = QByteArray();
	response.reserve(256 + body.size());
	response.append("HTTP/1.1 ")
		.append(QByteArray::number(status))
		.append(' ')
		.append(StatusText(status))
		.append("\r\nContent-Type: ")
		.append(contentType)
		.append("\r\nContent-Length: ")
		.append(QByteArray::number(body.size()))
		.append("\r\nCache-Control: no-store\r\nConnection: ")
		.append(connection->closeAfter ? "close" : "keep-alive")
		.append("\r\n")
		.append(extraHeaders)
		.append("\r\n")
		.append(body);
	socket->write(response);
	connection->lastActivity = crl::now();
	if (connection->closeAfter) {
		socket->disconnectFromHost();
	}
}

void Server::openStream(uint64 id) {
	const auto connection = find(id);
	if (!connection || !connection->socket) {
		return;
	}
	connection->stream = true;
	connection->buffer.clear();
	connection->socket->write(
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: keep-alive\r\n"
		"\r\n"
		": connected\n\n");
	refreshStreamsCount();
}

void Server::broadcast(const QJsonObject &message) {
	const auto data = "data: "
		+ QJsonDocument(message).toJson(QJsonDocument::Compact)
		+ "\n\n";
	for (const auto &[id, connection] : _connections) {
		if (connection->stream && connection->socket) {
			connection->socket->write(data);
		}
	}
}

void Server::sendKeepAlive() {
	const auto now = crl::now();
	auto idle = std::vector<uint64>();
	for (const auto &[id, connection] : _connections) {
		if (!connection->socket) {
			idle.push_back(id);
		} else if (connection->stream) {
			// Comments keep proxies and the client from timing the stream
			// out, and turn a dead peer into a write error.
			connection->socket->write(": ping\n\n");
		} else if (!connection->busy
			&& (now - connection->lastActivity > kIdleTimeout)) {
			idle.push_back(id);
		}
	}
	for (const auto id : idle) {
		if (const auto connection = find(id)) {
			if (const auto socket = connection->socket.data()) {
				socket->disconnectFromHost();
			} else {
				close(id);
			}
		}
	}
}

void Server::close(uint64 id) {
	const auto i = _connections.find(id);
	if (i == _connections.end()) {
		return;
	}
	auto connection = std::move(i->second);
	_connections.erase(i);
	if (const auto socket = connection->socket.data()) {
		socket->disconnect();
		socket->deleteLater();
	}
	refreshStreamsCount();
}

void Server::refreshStreamsCount() {
	auto count = 0;
	for (const auto &[id, connection] : _connections) {
		if (connection->stream) {
			++count;
		}
	}
	_streams = count;
}

rpl::producer<int> Server::streamsValue() const {
	return _streams.value();
}

} // namespace Mcp
