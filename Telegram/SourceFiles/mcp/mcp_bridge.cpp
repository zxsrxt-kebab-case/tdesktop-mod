/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mcp/mcp_bridge.h"

#include "mcp/mcp_settings.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else // Q_OS_WIN
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif // Q_OS_WIN

namespace Mcp {
namespace {

constexpr auto kBridgeArgument = "-mcpbridge";
constexpr auto kInitializeAttempts = 20;
constexpr auto kInitializeRetryDelay = std::chrono::milliseconds(500);
constexpr auto kStreamRetryDelay = std::chrono::seconds(2);
constexpr auto kDrainTimeout = std::chrono::seconds(5);
constexpr auto kDrainCheck = std::chrono::milliseconds(20);
constexpr auto kMaxHeaderSize = size_t(64 * 1024);
constexpr auto kReadChunk = 16 * 1024;

#ifdef Q_OS_WIN
using Socket = SOCKET;
const auto kInvalidSocket = INVALID_SOCKET;

void CloseSocket(Socket socket) {
	closesocket(socket);
}
#else // Q_OS_WIN
using Socket = int;
const auto kInvalidSocket = -1;

void CloseSocket(Socket socket) {
	::close(socket);
}
#endif // Q_OS_WIN

struct Response {
	int status = 0;
	std::string body;
};

std::mutex OutputMutex;
std::atomic<bool> StreamStarted = false;
std::atomic<int> Active = 0;
int Port = kDefaultPort;
std::string Token;

void WriteLine(std::string line) {
	line.push_back('\n');
	const auto lock = std::lock_guard(OutputMutex);
	auto offset = size_t(0);
	while (offset < line.size()) {
#ifdef Q_OS_WIN
		auto written = DWORD(0);
		const auto ok = WriteFile(
			GetStdHandle(STD_OUTPUT_HANDLE),
			line.data() + offset,
			DWORD(line.size() - offset),
			&written,
			nullptr);
		if (!ok || !written) {
			std::_Exit(0); // The client closed our stdout, it is gone.
		}
#else // Q_OS_WIN
		const auto written = ::write(
			1,
			line.data() + offset,
			line.size() - offset);
		if (written <= 0) {
			std::_Exit(0);
		}
#endif // Q_OS_WIN
		offset += size_t(written);
	}
}

[[nodiscard]] bool ReadLine(std::string &line) {
	static auto buffer = std::string();
	while (true) {
		const auto newline = buffer.find('\n');
		if (newline != std::string::npos) {
			line = buffer.substr(0, newline);
			buffer.erase(0, newline + 1);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			return true;
		}
		char chunk[kReadChunk];
#ifdef Q_OS_WIN
		auto read = DWORD(0);
		const auto ok = ReadFile(
			GetStdHandle(STD_INPUT_HANDLE),
			chunk,
			DWORD(sizeof(chunk)),
			&read,
			nullptr);
		if (!ok || !read) {
#else // Q_OS_WIN
		const auto read = ::read(0, chunk, sizeof(chunk));
		if (read <= 0) {
#endif // Q_OS_WIN
			line = std::move(buffer);
			buffer.clear();
			return !line.empty();
		}
		buffer.append(chunk, size_t(read));
	}
}

[[nodiscard]] Socket Connect() {
	const auto result = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (result == kInvalidSocket) {
		return result;
	}
	auto address = sockaddr_in();
	address.sin_family = AF_INET;
	address.sin_port = htons(uint16(Port));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const auto raw = reinterpret_cast<const sockaddr*>(&address);
	if (::connect(result, raw, sizeof(address)) != 0) {
		CloseSocket(result);
		return kInvalidSocket;
	}
	return result;
}

[[nodiscard]] bool SendAll(Socket socket, const std::string &data) {
	auto offset = size_t(0);
	while (offset < data.size()) {
		const auto sent = ::send(
			socket,
			data.data() + offset,
			int(data.size() - offset),
			0);
		if (sent <= 0) {
			return false;
		}
		offset += size_t(sent);
	}
	return true;
}

[[nodiscard]] int Receive(Socket socket, char *buffer, int size) {
	return int(::recv(socket, buffer, size, 0));
}

[[nodiscard]] std::string Lowercase(std::string text) {
	for (auto &ch : text) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return text;
}

[[nodiscard]] std::string Headers(const std::string &firstLine) {
	return firstLine
		+ "\r\nHost: 127.0.0.1:" + std::to_string(Port)
		+ "\r\nAuthorization: Bearer " + Token
		+ "\r\n";
}

[[nodiscard]] std::optional<Response> Post(const std::string &body) {
	const auto socket = Connect();
	if (socket == kInvalidSocket) {
		return std::nullopt;
	}
	const auto request = Headers("POST /mcp HTTP/1.1")
		+ "Content-Type: application/json\r\n"
		+ "Accept: application/json, text/event-stream\r\n"
		+ "Content-Length: " + std::to_string(body.size()) + "\r\n"
		+ "Connection: close\r\n"
		+ "\r\n"
		+ body;
	if (!SendAll(socket, request)) {
		CloseSocket(socket);
		return std::nullopt;
	}
	auto data = std::string();
	auto headerEnd = std::string::npos;
	char chunk[kReadChunk];
	while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
		const auto read = Receive(socket, chunk, sizeof(chunk));
		if (read <= 0 || data.size() > kMaxHeaderSize) {
			CloseSocket(socket);
			return std::nullopt;
		}
		data.append(chunk, size_t(read));
	}
	auto result = Response();
	const auto space = data.find(' ');
	if (space != std::string::npos) {
		result.status = std::atoi(data.c_str() + space + 1);
	}
	const auto headers = Lowercase(data.substr(0, headerEnd + 2));
	const auto lengthAt = headers.find("\r\ncontent-length:");
	const auto length = (lengthAt != std::string::npos)
		? size_t(std::atoll(headers.c_str() + lengthAt + 17))
		: size_t(0);
	result.body = data.substr(headerEnd + 4);
	while (result.body.size() < length) {
		const auto read = Receive(socket, chunk, sizeof(chunk));
		if (read <= 0) {
			break;
		}
		result.body.append(chunk, size_t(read));
	}
	CloseSocket(socket);
	if (result.body.size() > length) {
		result.body.resize(length);
	}
	return result;
}

[[nodiscard]] std::string ErrorLine(
		const QJsonValue &id,
		int code,
		const QString &message) {
	const auto json = QJsonObject{
		{ u"jsonrpc"_q, u"2.0"_q },
		{ u"id"_q, id },
		{ u"error"_q, QJsonObject{
			{ u"code"_q, code },
			{ u"message"_q, message },
		} },
	};
	return QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();
}

// Server-Sent Events: "data:" lines make up an event, an empty line ends
// it. Comments (the keep-alive pings) start with a colon and are skipped.
void ReadStream(Socket socket) {
	auto buffer = std::string();
	auto event = std::string();
	auto started = false;
	char chunk[kReadChunk];
	while (true) {
		const auto read = Receive(socket, chunk, sizeof(chunk));
		if (read <= 0) {
			return;
		}
		buffer.append(chunk, size_t(read));
		if (!started) {
			const auto end = buffer.find("\r\n\r\n");
			if (end == std::string::npos) {
				if (buffer.size() > kMaxHeaderSize) {
					return;
				}
				continue;
			}
			const auto space = buffer.find(' ');
			if (space == std::string::npos
				|| std::atoi(buffer.c_str() + space + 1) != 200) {
				return;
			}
			buffer.erase(0, end + 4);
			started = true;
		}
		auto newline = std::string::npos;
		while ((newline = buffer.find('\n')) != std::string::npos) {
			auto line = buffer.substr(0, newline);
			buffer.erase(0, newline + 1);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				if (!event.empty()) {
					WriteLine(std::move(event));
					event.clear();
				}
			} else if (line.rfind("data:", 0) == 0) {
				auto value = line.substr(5);
				if (!value.empty() && value.front() == ' ') {
					value.erase(0, 1);
				}
				if (!event.empty()) {
					event.push_back('\n');
				}
				event += value;
			}
		}
	}
}

// Notifications are only forwarded once the client got its answer to
// initialize, and the stream comes back by itself when the app restarts.
void StartStream() {
	if (StreamStarted.exchange(true)) {
		return;
	}
	std::thread([] {
		while (true) {
			const auto socket = Connect();
			if (socket != kInvalidSocket) {
				const auto request = Headers("GET /mcp HTTP/1.1")
					+ "Accept: text/event-stream\r\n"
					+ "\r\n";
				if (SendAll(socket, request)) {
					ReadStream(socket);
				}
				CloseSocket(socket);
			}
			std::this_thread::sleep_for(kStreamRetryDelay);
		}
	}).detach();
}

void HandleLine(std::string line) {
	const auto document = QJsonDocument::fromJson(
		QByteArray::fromStdString(line));
	const auto message = document.object();
	const auto hasId = message.contains(u"id"_q);
	const auto id = message.value(u"id"_q);
	const auto initialize = (message.value(u"method"_q).toString()
		== u"initialize"_q);

	// The client may be launched together with the app, give it a moment.
	const auto attempts = initialize ? kInitializeAttempts : 1;
	auto response = std::optional<Response>();
	for (auto i = 0; i != attempts && !response; ++i) {
		if (i) {
			std::this_thread::sleep_for(kInitializeRetryDelay);
		}
		response = Post(line);
	}
	if (!response) {
		if (hasId) {
			WriteLine(ErrorLine(
				id,
				-32000,
				u"KebabClient is not running, or its MCP server is off."_q));
		}
		return;
	} else if (response->status == 200) {
		if (!response->body.empty()) {
			WriteLine(std::move(response->body));
		}
		if (initialize) {
			StartStream();
		}
	} else if (response->status != 202 && hasId) {
		WriteLine(ErrorLine(
			id,
			-32001,
			(response->status == 401)
				? u"KebabClient refused the MCP token. Copy the current one "
					"from Settings > Modifications > MCP."_q
				: (u"KebabClient answered HTTP "_q
					+ QString::number(response->status)
					+ '.')));
	}
}

} // namespace

bool IsBridgeInvocation(int argc, char *argv[]) {
	return (argc >= 2) && !std::strcmp(argv[1], kBridgeArgument);
}

int RunBridge(int argc, char *argv[]) {
#ifdef Q_OS_WIN
	auto wsa = WSADATA();
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return 1;
	}
#endif // Q_OS_WIN

	const auto port = (argc >= 3)
		? std::atoi(argv[2])
		: qEnvironmentVariableIntValue("KEBAB_MCP_PORT");
	Port = (port >= kMinPort && port <= kMaxPort) ? port : kDefaultPort;
	Token = qEnvironmentVariable("KEBAB_MCP_TOKEN").trimmed().toStdString();

	auto line = std::string();
	while (ReadLine(line)) {
		if (line.find_first_not_of(" \t") == std::string::npos) {
			continue;
		}
		// Each request gets its own thread, so a long poll_events does not
		// hold up the requests behind it.
		++Active;
		std::thread([](std::string line) {
			HandleLine(std::move(line));
			--Active;
		}, std::move(line)).detach();
		line = std::string();
	}

	// A client may close stdin right after its last request, so what is in
	// flight gets a moment to answer. Anything slower, like a long poll, has
	// nobody left to answer to.
	const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
	while (Active > 0 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(kDrainCheck);
	}
	const auto lock = std::lock_guard(OutputMutex);
	std::_Exit(0);
}

} // namespace Mcp
