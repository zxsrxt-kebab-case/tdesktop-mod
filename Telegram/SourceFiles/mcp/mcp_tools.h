/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "mcp/mcp_settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

class PeerData;

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Mcp {

class Events;

struct ToolResult {
	QJsonValue data;
	QString error;
};

// The MCP tools. Each belongs to a capability and disappears from the list
// while it is off. Every call is answered exactly once: a request lost with
// its session, or one the server never answers, ends in a timeout instead
// of leaving the client waiting.
class Tools final : public base::has_weak_ptr {
public:
	Tools(not_null<Settings*> settings, not_null<Events*> events);
	~Tools();

	[[nodiscard]] QJsonArray list() const;
	void call(
		const QString &name,
		const QJsonObject &arguments,
		Fn<void(ToolResult)> done);

private:
	using Done = Fn<void(ToolResult)>;
	using Handler = Fn<void(const QJsonObject &arguments, Done done)>;

	struct Tool {
		QString name;
		QString description;
		QJsonObject schema;
		Capability capability = Capability::Read;
		Handler handler;
	};

	struct Target {
		int account = 0;
		not_null<Main::Session*> session;
	};

	struct PendingSend;

	void add(
		QString name,
		Capability capability,
		QString description,
		QJsonObject schema,
		Handler handler);
	void registerTools();

	[[nodiscard]] std::optional<Target> resolveAccount(
		const QJsonObject &arguments,
		QString *error) const;
	void resolvePeer(
		not_null<Main::Session*> session,
		const QJsonValue &chat,
		Fn<void(PeerData *peer, QString error)> done);
	void withPeer(
		const QJsonObject &arguments,
		Done done,
		Fn<void(Target target, not_null<PeerData*> peer)> callback);
	void withThread(
		not_null<PeerData*> peer,
		MsgId topic,
		Done done,
		Fn<void(not_null<Data::Thread*> thread)> callback);

	void waitSent(
		not_null<Main::Session*> session,
		FullMsgId localId,
		QJsonObject chat,
		Done done);
	void finishSend(not_null<PendingSend*> pending, ToolResult result);
	void checkSends();

	void listAccounts(const QJsonObject &arguments, Done done);
	void listChats(const QJsonObject &arguments, Done done);
	void getChat(const QJsonObject &arguments, Done done);
	void listTopics(const QJsonObject &arguments, Done done);
	void readHistory(const QJsonObject &arguments, Done done);
	void searchMessages(const QJsonObject &arguments, Done done);
	void messageVersions(const QJsonObject &arguments, Done done);
	void setDraft(const QJsonObject &arguments, Done done);
	void sendMessage(const QJsonObject &arguments, Done done);
	void createTopic(const QJsonObject &arguments, Done done);
	void markRead(const QJsonObject &arguments, Done done);
	void editMessage(const QJsonObject &arguments, Done done);
	void deleteMessages(const QJsonObject &arguments, Done done);
	void getClientSettings(const QJsonObject &arguments, Done done);
	void setClientSetting(const QJsonObject &arguments, Done done);
	void subscribe(const QJsonObject &arguments, Done done);
	void unsubscribe(const QJsonObject &arguments, Done done);
	void listSubscriptions(const QJsonObject &arguments, Done done);
	void pollEvents(const QJsonObject &arguments, Done done);

	const not_null<Settings*> _settings;
	const not_null<Events*> _events;
	std::vector<Tool> _tools;

	base::flat_map<uint64, std::unique_ptr<PendingSend>> _sends;
	uint64 _lastSendId = 0;
	base::Timer _sendsTimer;

};

} // namespace Mcp
