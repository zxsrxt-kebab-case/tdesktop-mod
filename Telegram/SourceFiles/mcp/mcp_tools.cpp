/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mcp/mcp_tools.h"

#include "api/api_common.h"
#include "api/api_editing.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/streamer_mode.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_drafts.h"
#include "data/data_folder.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_message_versions.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_common.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_unread_things.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mcp/mcp_events.h"
#include "mcp/mcp_format.h"
#include "storage/storage_account.h"
#include "ui/item_text_options.h"
#include "ui/text/text.h"

#include <QtCore/QRegularExpression>

namespace Mcp {
namespace {

// Longer than the longest poll_events wait, so a poll always answers first.
constexpr auto kCallTimeout = 30 * crl::time(1000);
constexpr auto kSendTimeout = 20 * crl::time(1000);
constexpr auto kSendCheckInterval = crl::time(250);
constexpr auto kDefaultLimit = 30;
constexpr auto kMaxLimit = 100;
constexpr auto kDefaultPollEvents = 50;
constexpr auto kMaxPollEvents = 200;
constexpr auto kMaxPollSeconds = 25;
constexpr auto kMaxDeleteIds = 100;
constexpr auto kPreviewLength = 300;

struct ChatReference {
	std::optional<PeerId> id;
	QString username;
	bool self = false;
	bool valid = false;
};

struct ParsedMessages {
	QJsonArray messages;
	std::optional<int> count;
	int nextRate = 0;
	PeerId lastPeer = 0;
	MsgId lastId = 0;
};

[[nodiscard]] ToolResult Error(const QString &text) {
	return { .error = text };
}

[[nodiscard]] QString ApiError(const MTP::Error &error) {
	return u"Telegram refused the request: "_q + error.type() + '.';
}

[[nodiscard]] std::optional<qint64> IntArgument(
		const QJsonObject &arguments,
		const QString &name) {
	const auto value = arguments.value(name);
	if (value.isDouble()) {
		return qint64(value.toDouble());
	} else if (value.isString()) {
		auto ok = false;
		const auto result = value.toString().trimmed().toLongLong(&ok);
		if (ok) {
			return result;
		}
	}
	return std::nullopt;
}

[[nodiscard]] int LimitArgument(
		const QJsonObject &arguments,
		const QString &name,
		int fallback,
		int max) {
	const auto value = IntArgument(arguments, name);
	return value
		? int(std::clamp(*value, qint64(1), qint64(max)))
		: fallback;
}

[[nodiscard]] MsgId IdArgument(
		const QJsonObject &arguments,
		const QString &name) {
	const auto value = IntArgument(arguments, name).value_or(0);
	return (value > 0 && value <= std::numeric_limits<int32>::max())
		? MsgId(value)
		: MsgId();
}

[[nodiscard]] QString StringArgument(
		const QJsonObject &arguments,
		const QString &name) {
	return arguments.value(name).toString();
}

[[nodiscard]] bool BoolArgument(
		const QJsonObject &arguments,
		const QString &name,
		bool fallback) {
	const auto value = arguments.value(name);
	return value.isBool() ? value.toBool() : fallback;
}

[[nodiscard]] QString Preview(const QString &text) {
	return (text.size() > kPreviewLength)
		? (text.left(kPreviewLength) + QChar(0x2026))
		: text;
}

[[nodiscard]] QJsonObject ItemPreview(not_null<HistoryItem*> item) {
	auto result = ItemJson(item);
	result[u"text"_q] = Preview(result[u"text"_q].toString());
	return result;
}

[[nodiscard]] Dialogs::UnreadState Unread(not_null<Dialogs::Entry*> entry) {
	return entry->chatListUnreadState();
}

[[nodiscard]] ChatReference ParseChat(const QJsonValue &value) {
	auto result = ChatReference();
	const auto text = value.isDouble()
		? QString::number(qint64(value.toDouble()))
		: value.toString().trimmed();
	if (text.isEmpty()) {
		return result;
	}
	for (const auto self : { u"me"_q, u"self"_q, u"saved"_q }) {
		if (!text.compare(self, Qt::CaseInsensitive)) {
			result.self = result.valid = true;
			return result;
		}
	}
	auto ok = false;
	const auto number = text.toLongLong(&ok);
	if (ok) {
		result.id = PeerFromBotApi(number);
		result.valid = result.id.has_value();
		return result;
	}
	static const auto kLink = QRegularExpression(
		u"^(?:https?://)?(?:www\\.)?(?:t\\.me|telegram\\.me|telegram\\.dog)/"
		u"(?:c/(\\d+)|([A-Za-z0-9_]+))(?:[/?#].*)?$"_q,
		QRegularExpression::CaseInsensitiveOption);
	if (const auto match = kLink.match(text); match.hasMatch()) {
		if (const auto channel = match.captured(1); !channel.isEmpty()) {
			const auto bare = channel.toULongLong();
			if (bare) {
				result.id = peerFromChannel(ChannelId(bare));
				result.valid = true;
			}
		} else {
			result.username = match.captured(2);
			result.valid = true;
		}
		return result;
	}
	static const auto kUsername = QRegularExpression(
		u"^@?([A-Za-z0-9_]{3,64})$"_q);
	if (const auto match = kUsername.match(text); match.hasMatch()) {
		result.username = match.captured(1);
		result.valid = true;
	}
	return result;
}

[[nodiscard]] PeerId MessagePeer(const MTPMessage &message) {
	return message.match([](const MTPDmessageEmpty &data) {
		return data.vpeer_id() ? peerFromMTP(*data.vpeer_id()) : PeerId();
	}, [](const auto &data) {
		return peerFromMTP(data.vpeer_id());
	});
}

[[nodiscard]] ParsedMessages ParseMessages(
		not_null<Main::Session*> session,
		const MTPmessages_Messages &result,
		bool withChat) {
	auto parsed = ParsedMessages();
	const auto process = [&](const auto &data) {
		// Only the peers are registered, the way any response does it; the
		// messages stay out of the loaded history.
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		for (const auto &message : data.vmessages().v) {
			const auto peerId = MessagePeer(message);
			const auto peer = session->data().peerLoaded(peerId);
			auto json = MessageJson(
				session,
				message,
				peer && peer->isForum());
			if (json.isEmpty()) {
				continue;
			} else if (withChat) {
				json[u"chat"_q] = peer
					? PeerJson(peer)
					: PeerRef(session, peerId);
			}
			parsed.lastPeer = peerId;
			parsed.lastId = MsgId(json[u"id"_q].toInteger());
			parsed.messages.push_back(json);
		}
	};
	result.match([&](const MTPDmessages_messages &data) {
		process(data);
	}, [&](const MTPDmessages_messagesSlice &data) {
		process(data);
		parsed.count = data.vcount().v;
		parsed.nextRate = data.vnext_rate().value_or_empty();
	}, [&](const MTPDmessages_channelMessages &data) {
		process(data);
		parsed.count = data.vcount().v;
	}, [](const MTPDmessages_messagesNotModified &) {
	});
	return parsed;
}

[[nodiscard]] QJsonObject ChatEntry(not_null<History*> history) {
	auto result = PeerJson(history->peer);
	const auto state = Unread(history);
	result[u"unread"_q] = state.messages;
	if (history->unreadMark()) {
		result[u"marked_unread"_q] = true;
	}
	if (const auto mentions = history->unreadMentions().count(0)) {
		result[u"mentions"_q] = mentions;
	}
	if (history->muted()) {
		result[u"muted"_q] = true;
	}
	if (history->isPinnedDialog(FilterId())) {
		result[u"pinned"_q] = true;
	}
	if (const auto last = history->lastMessage()) {
		result[u"last_message"_q] = ItemPreview(last);
	}
	return result;
}

[[nodiscard]] QJsonObject SubscriptionJson(const Subscription &subscription) {
	auto events = QJsonArray();
	for (const auto type : {
			EventType::Message,
			EventType::Mention,
			EventType::TopicCreated,
			EventType::Edited }) {
		if (subscription.types & type) {
			events.push_back(EventTypeName(type));
		}
	}
	auto result = QJsonObject{
		{ u"id"_q, subscription.id },
		{ u"events"_q, events },
		{ u"include_outgoing"_q, subscription.includeOutgoing },
	};
	if (subscription.account >= 0) {
		result[u"account"_q] = subscription.account;
	}
	if (subscription.peer) {
		result[u"chat"_q] = BotApiId(subscription.peer);
	}
	if (subscription.topic) {
		result[u"topic"_q] = qint64(subscription.topic.bare);
	}
	if (subscription.from) {
		result[u"from"_q] = BotApiId(subscription.from);
	}
	return result;
}

[[nodiscard]] MsgId SentMessageId(const MTPUpdates &updates, uint64 randomId) {
	auto result = MsgId();
	const auto check = [&](const MTPVector<MTPUpdate> &list) {
		for (const auto &update : list.v) {
			if (update.type() == mtpc_updateMessageID) {
				const auto &data = update.c_updateMessageID();
				if (data.vrandom_id().v == randomId) {
					result = data.vid().v;
				}
			}
		}
	};
	updates.match([&](const MTPDupdates &data) {
		check(data.vupdates());
	}, [&](const MTPDupdatesCombined &data) {
		check(data.vupdates());
	}, [](const auto &) {
	});
	return result;
}

struct ClientSetting {
	QString name;
	QString description;
	Fn<bool()> get;
	Fn<void(bool)> set;
};

[[nodiscard]] std::vector<ClientSetting> ClientSettings() {
	auto result = std::vector<ClientSetting>{
		{
			u"ghost_mode"_q,
			u"Hide online status, typing and read receipts."_q,
			[] { return Core::App().settings().ghostMode(); },
			[](bool value) { Core::App().settings().setGhostMode(value); },
		},
		{
			u"save_message_versions"_q,
			u"Keep local copies of edited and deleted messages."_q,
			[] { return Core::App().settings().saveMessageVersions(); },
			[](bool value) {
				Core::App().settings().setSaveMessageVersions(value);
			},
		},
		{
			u"message_time_seconds"_q,
			u"Show seconds in message times."_q,
			[] { return Core::App().settings().messageTimeSeconds(); },
			[](bool value) {
				Core::App().settings().setMessageTimeSeconds(value);
			},
		},
		{
			u"unlock_restricted_content"_q,
			u"Allow saving and forwarding from protected chats."_q,
			[] { return Core::App().settings().unlockRestrictedContent(); },
			[](bool value) {
				Core::App().settings().setUnlockRestrictedContent(value);
			},
		},
		{
			u"instant_notifications"_q,
			u"Show notifications without the usual delay."_q,
			[] { return Core::App().settings().instantNotifications(); },
			[](bool value) {
				Core::App().settings().setInstantNotifications(value);
			},
		},
		{
			u"compact_notifications"_q,
			u"Use the small notification popups."_q,
			[] { return Core::App().settings().compactNotifications(); },
			[](bool value) {
				Core::App().settings().setCompactNotifications(value);
			},
		},
	};
	if (Core::StreamerMode::Supported()) {
		result.push_back({
			u"streamer_mode"_q,
			u"Hide windows from screen capture."_q,
			[] { return Core::App().settings().streamerMode(); },
			[](bool value) { Core::App().settings().setStreamerMode(value); },
		});
	}
	return result;
}

[[nodiscard]] QJsonObject Property(
		const QString &type,
		const QString &description) {
	return QJsonObject{
		{ u"type"_q, type },
		{ u"description"_q, description },
	};
}

[[nodiscard]] QJsonObject AccountProperty() {
	return Property(
		u"integer"_q,
		u"Account index from list_accounts. "
		"Defaults to the active account."_q);
}

[[nodiscard]] QJsonObject ChatProperty(const QString &description) {
	return QJsonObject{
		{ u"type"_q, QJsonArray{ u"string"_q, u"integer"_q } },
		{ u"description"_q, description },
	};
}

[[nodiscard]] QJsonObject ChatProperty() {
	return ChatProperty(
		u"The chat: a Bot API id (-1001234567890 for a supergroup or a "
		"channel), an @username, a t.me link, or \"me\" for Saved "
		"Messages."_q);
}

[[nodiscard]] QJsonObject TopicProperty(const QString &description) {
	return Property(u"integer"_q, description);
}

[[nodiscard]] QJsonObject Schema(
		std::vector<std::pair<QString, QJsonObject>> properties,
		QStringList required = {}) {
	auto map = QJsonObject();
	for (auto &[name, property] : properties) {
		map.insert(name, property);
	}
	auto result = QJsonObject{
		{ u"type"_q, u"object"_q },
		{ u"properties"_q, map },
	};
	if (!required.isEmpty()) {
		result.insert(u"required"_q, QJsonArray::fromStringList(required));
	}
	return result;
}

} // namespace

struct Tools::PendingSend {
	base::weak_ptr<Main::Session> session;
	FullMsgId localId;
	QJsonObject chat;
	Done done;
	crl::time started = 0;
	bool finished = false;
	rpl::lifetime lifetime;
};

Tools::Tools(not_null<Settings*> settings, not_null<Events*> events)
: _settings(settings)
, _events(events)
, _sendsTimer([=] { checkSends(); }) {
	registerTools();
}

Tools::~Tools() {
	for (const auto &[id, pending] : base::take(_sends)) {
		if (!pending->finished) {
			pending->done(Error(u"The server stopped."_q));
		}
	}
}

QJsonArray Tools::list() const {
	auto result = QJsonArray();
	for (const auto &tool : _tools) {
		if (!_settings->has(tool.capability)) {
			continue;
		}
		const auto readOnly = (tool.capability == Capability::Read)
			|| (tool.name == u"get_client_settings"_q)
			|| (tool.name == u"list_subscriptions"_q)
			|| (tool.name == u"poll_events"_q);
		result.push_back(QJsonObject{
			{ u"name"_q, tool.name },
			{ u"description"_q, tool.description },
			{ u"inputSchema"_q, tool.schema },
			{ u"annotations"_q, QJsonObject{
				{ u"readOnlyHint"_q, readOnly },
				{ u"destructiveHint"_q, tool.capability == Capability::Manage },
				{ u"openWorldHint"_q, false },
			} },
		});
	}
	return result;
}

void Tools::call(
		const QString &name,
		const QJsonObject &arguments,
		Fn<void(ToolResult)> done) {
	const auto i = ranges::find(_tools, name, &Tool::name);
	if (i == _tools.end() || !_settings->has(i->capability)) {
		done(Error(u"There is no tool \""_q + name + u"\"."_q));
		return;
	}
	const auto answered = std::make_shared<bool>(false);
	const auto once = [=](ToolResult result) {
		if (!*answered) {
			*answered = true;
			done(std::move(result));
		}
	};
	base::call_delayed(kCallTimeout, this, [=] {
		once(Error(u"Timed out waiting for Telegram."_q));
	});
	const auto handler = i->handler;
	handler(arguments, once);
}

void Tools::add(
		QString name,
		Capability capability,
		QString description,
		QJsonObject schema,
		Handler handler) {
	_tools.push_back({
		.name = std::move(name),
		.description = std::move(description),
		.schema = std::move(schema),
		.capability = capability,
		.handler = std::move(handler),
	});
}

void Tools::registerTools() {
	using Method = void(Tools::*)(const QJsonObject&, Done);
	const auto bind = [=](Method method) {
		return [=](const QJsonObject &arguments, Done done) {
			(this->*method)(arguments, std::move(done));
		};
	};
	const auto chatOnly = [] {
		return Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
		}, { u"chat"_q });
	};

	add(
		u"list_accounts"_q,
		Capability::Read,
		u"Lists the Telegram accounts logged in to this client. Every other "
		"tool takes an optional account index from here."_q,
		Schema({}),
		bind(&Tools::listAccounts));

	add(
		u"list_chats"_q,
		Capability::Read,
		u"Lists chats in chat-list order (pinned first), with unread "
		"counts and the last message. Only the chats the client has "
		"loaded are listed; \"complete\" says whether that is all of "
		"them."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"limit"_q, Property(u"integer"_q, u"1-100, default 30."_q) },
			{ u"offset"_q, Property(u"integer"_q, u"Skip this many."_q) },
			{ u"unread_only"_q, Property(u"boolean"_q, u"Only unread."_q) },
			{ u"query"_q, Property(
				u"string"_q,
				u"Filter by title or username."_q) },
			{ u"folder"_q, QJsonObject{
				{ u"type"_q, u"string"_q },
				{ u"enum"_q, QJsonArray{ u"main"_q, u"archive"_q } },
				{ u"description"_q, u"Default main."_q },
			} },
		}),
		bind(&Tools::listChats));

	add(
		u"get_chat"_q,
		Capability::Read,
		u"Describes one chat: type, description, members, unread state, "
		"whether you can write there and whether it is a forum."_q,
		chatOnly(),
		bind(&Tools::getChat));

	add(
		u"list_topics"_q,
		Capability::Read,
		u"Lists the topics of a forum group, most recently active first, "
		"with unread counts and the last message of each. Topic 1 is "
		"General."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"query"_q, Property(u"string"_q, u"Filter by title."_q) },
			{ u"limit"_q, Property(u"integer"_q, u"1-100, default 30."_q) },
			{ u"offset"_q, Property(
				u"string"_q,
				u"next_offset from the previous page."_q) },
		}, { u"chat"_q }),
		bind(&Tools::listTopics));

	add(
		u"read_history"_q,
		Capability::Read,
		u"Reads messages of a chat or of one forum topic, newest first. "
		"Does not mark anything as read. Without a topic a forum gives "
		"all its topics mixed, each message naming its topic."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"topic"_q, TopicProperty(u"Forum topic id; 1 is General."_q) },
			{ u"limit"_q, Property(u"integer"_q, u"1-100, default 30."_q) },
			{ u"before_id"_q, Property(
				u"integer"_q,
				u"Only messages older than this id."_q) },
			{ u"after_id"_q, Property(
				u"integer"_q,
				u"Only messages newer than this id."_q) },
		}, { u"chat"_q }),
		bind(&Tools::readHistory));

	add(
		u"search_messages"_q,
		Capability::Read,
		u"Searches messages in one chat or topic, or across all chats "
		"when no chat is given."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"query"_q, Property(u"string"_q, u"Text to look for."_q) },
			{ u"chat"_q, ChatProperty(
				u"Limit to this chat (Bot API id, @username, t.me link "
				"or \"me\")."_q) },
			{ u"topic"_q, TopicProperty(u"Limit to this forum topic."_q) },
			{ u"from"_q, ChatProperty(
				u"Only messages from this user (needs chat)."_q) },
			{ u"limit"_q, Property(u"integer"_q, u"1-100, default 30."_q) },
			{ u"offset"_q, Property(
				u"string"_q,
				u"next_offset from the previous page."_q) },
		}, { u"query"_q }),
		bind(&Tools::searchMessages));

	add(
		u"message_versions"_q,
		Capability::Read,
		u"Earlier texts of an edited or deleted message, as saved by this "
		"client while \"save message versions\" was on."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"message_id"_q, Property(u"integer"_q, u"Message id."_q) },
		}, { u"chat"_q, u"message_id"_q }),
		bind(&Tools::messageVersions));

	add(
		u"set_draft"_q,
		Capability::Drafts,
		u"Puts text into the message field of a chat or topic without "
		"sending it, for the user to review. Empty text clears the "
		"draft. In a forum the default topic is General."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"topic"_q, TopicProperty(u"Forum topic id; 1 is General."_q) },
			{ u"text"_q, Property(u"string"_q, u"Draft text."_q) },
			{ u"reply_to"_q, Property(
				u"integer"_q,
				u"Message id to reply to."_q) },
		}, { u"chat"_q, u"text"_q }),
		bind(&Tools::setDraft));

	add(
		u"send_message"_q,
		Capability::Send,
		u"Sends a text message as the user. In a forum it goes to the "
		"given topic, or to General. Answers with the new message id."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"topic"_q, TopicProperty(u"Forum topic id; 1 is General."_q) },
			{ u"text"_q, Property(u"string"_q, u"Message text."_q) },
			{ u"reply_to"_q, Property(
				u"integer"_q,
				u"Message id to reply to."_q) },
			{ u"silent"_q, Property(
				u"boolean"_q,
				u"Send without a notification sound."_q) },
		}, { u"chat"_q, u"text"_q }),
		bind(&Tools::sendMessage));

	add(
		u"create_topic"_q,
		Capability::Send,
		u"Creates a topic in a forum group and answers with its id."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"title"_q, Property(u"string"_q, u"Topic title."_q) },
		}, { u"chat"_q, u"title"_q }),
		bind(&Tools::createTopic));

	add(
		u"mark_read"_q,
		Capability::Send,
		u"Marks a chat or a forum topic as read. Sends the read receipt "
		"even in ghost mode."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"topic"_q, TopicProperty(u"Forum topic id; 1 is General."_q) },
		}, { u"chat"_q }),
		bind(&Tools::markRead));

	add(
		u"edit_message"_q,
		Capability::Manage,
		u"Replaces the text of a message the user sent."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"message_id"_q, Property(u"integer"_q, u"Message id."_q) },
			{ u"text"_q, Property(u"string"_q, u"New text."_q) },
		}, { u"chat"_q, u"message_id"_q, u"text"_q }),
		bind(&Tools::editMessage));

	add(
		u"delete_messages"_q,
		Capability::Manage,
		u"Deletes messages, for everyone by default."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty() },
			{ u"message_ids"_q, QJsonObject{
				{ u"type"_q, u"array"_q },
				{ u"items"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
				{ u"description"_q, u"Up to 100 message ids."_q },
			} },
			{ u"revoke"_q, Property(
				u"boolean"_q,
				u"Delete for everyone, default true."_q) },
		}, { u"chat"_q, u"message_ids"_q }),
		bind(&Tools::deleteMessages));

	add(
		u"get_client_settings"_q,
		Capability::Client,
		u"Shows this client's own switches: ghost mode, streamer mode and "
		"the like."_q,
		Schema({}),
		bind(&Tools::getClientSettings));

	add(
		u"set_client_setting"_q,
		Capability::Client,
		u"Turns one of this client's switches on or off; "
		"get_client_settings lists them."_q,
		Schema({
			{ u"name"_q, Property(u"string"_q, u"Setting name."_q) },
			{ u"value"_q, Property(u"boolean"_q, u"New value."_q) },
		}, { u"name"_q, u"value"_q }),
		bind(&Tools::setClientSetting));

	add(
		u"subscribe"_q,
		Capability::Events,
		u"Starts delivering events: new messages, mentions, created "
		"topics, edits. Every filter is optional; with none it follows "
		"every chat of every account. Events are pushed to the event "
		"stream (a Claude Code channel shows them as they come) and "
		"queued for poll_events. Subscriptions last until the client "
		"restarts or the server is turned off."_q,
		Schema({
			{ u"account"_q, AccountProperty() },
			{ u"chat"_q, ChatProperty(u"Only this chat."_q) },
			{ u"topic"_q, TopicProperty(u"Only this forum topic."_q) },
			{ u"from"_q, ChatProperty(u"Only messages from this user."_q) },
			{ u"events"_q, QJsonObject{
				{ u"type"_q, u"array"_q },
				{ u"items"_q, QJsonObject{
					{ u"type"_q, u"string"_q },
					{ u"enum"_q, QJsonArray{
						u"message"_q,
						u"mention"_q,
						u"topic_created"_q,
						u"edited"_q,
					} },
				} },
				{ u"description"_q, u"Default message and topic_created; "
					"message includes mentions."_q },
			} },
			{ u"include_outgoing"_q, Property(
				u"boolean"_q,
				u"Also the user's own messages."_q) },
		}),
		bind(&Tools::subscribe));

	add(
		u"unsubscribe"_q,
		Capability::Events,
		u"Stops a subscription, or all of them."_q,
		Schema({
			{ u"id"_q, Property(u"integer"_q, u"Subscription id."_q) },
			{ u"all"_q, Property(u"boolean"_q, u"Stop every one."_q) },
		}),
		bind(&Tools::unsubscribe));

	add(
		u"list_subscriptions"_q,
		Capability::Events,
		u"Lists the active subscriptions."_q,
		Schema({}),
		bind(&Tools::listSubscriptions));

	add(
		u"poll_events"_q,
		Capability::Events,
		u"Takes queued events, waiting up to timeout_seconds for the first "
		"one. For clients that do not read the event stream."_q,
		Schema({
			{ u"max"_q, Property(u"integer"_q, u"1-200, default 50."_q) },
			{ u"timeout_seconds"_q, Property(
				u"integer"_q,
				u"0-25, default 0."_q) },
		}),
		bind(&Tools::pollEvents));
}

std::optional<Tools::Target> Tools::resolveAccount(
		const QJsonObject &arguments,
		QString *error) const {
	auto &domain = Core::App().domain();
	if (!domain.started()) {
		*error = u"Telegram is still starting."_q;
		return std::nullopt;
	}
	auto account = (Main::Account*)nullptr;
	auto accountIndex = 0;
	if (arguments.contains(u"account"_q)) {
		const auto index = IntArgument(arguments, u"account"_q);
		for (const auto &[i, entry] : domain.accounts()) {
			if (index && i == *index) {
				account = entry.get();
				accountIndex = i;
			}
		}
		if (!account) {
			*error = u"There is no such account; see list_accounts."_q;
			return std::nullopt;
		}
	} else {
		account = &domain.active();
		for (const auto &[i, entry] : domain.accounts()) {
			if (entry.get() == account) {
				accountIndex = i;
			}
		}
	}
	if (!account->sessionExists()) {
		*error = u"That account is not logged in."_q;
		return std::nullopt;
	}
	return Target{ accountIndex, &account->session() };
}

void Tools::resolvePeer(
		not_null<Main::Session*> session,
		const QJsonValue &chat,
		Fn<void(PeerData *peer, QString error)> done) {
	const auto reference = ParseChat(chat);
	if (!reference.valid) {
		done(nullptr, u"Pass the chat as a Bot API id, an @username, "
			"a t.me link or \"me\"."_q);
		return;
	} else if (reference.self) {
		done(session->user().get(), QString());
		return;
	} else if (reference.id) {
		if (const auto peer = session->data().peerLoaded(*reference.id)) {
			done(peer, QString());
		} else {
			done(nullptr, u"This account does not know the chat "_q
				+ QString::number(BotApiId(*reference.id))
				+ u". Pass its @username, or open it in the client once."_q);
		}
		return;
	}
	const auto username = reference.username;
	if (const auto peer = session->data().peerByUsername(username)) {
		done(peer, QString());
		return;
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		done(
			peer,
			peer ? QString() : (u"Could not open @"_q + username + '.'));
	}).fail([=](const MTP::Error &error) {
		const auto type = error.type();
		done(
			nullptr,
			(type == u"USERNAME_NOT_OCCUPIED"_q
				|| type == u"USERNAME_INVALID"_q)
				? (u"Nobody has the username @"_q + username + '.')
				: ApiError(error));
	}).send();
}

void Tools::withPeer(
		const QJsonObject &arguments,
		Done done,
		Fn<void(Target target, not_null<PeerData*> peer)> callback) {
	auto error = QString();
	const auto target = resolveAccount(arguments, &error);
	if (!target) {
		done(Error(error));
		return;
	}
	resolvePeer(target->session, arguments.value(u"chat"_q), crl::guard(this, [=](
			PeerData *peer,
			QString error) {
		if (!peer) {
			done(Error(error));
		} else {
			callback(*target, peer);
		}
	}));
}

void Tools::withThread(
		not_null<PeerData*> peer,
		MsgId topic,
		Done done,
		Fn<void(not_null<Data::Thread*> thread)> callback) {
	const auto forum = peer->forum();
	if (!forum) {
		if (topic) {
			done(Error(u"This chat has no topics."_q));
		} else {
			callback(peer->owner().history(peer));
		}
		return;
	}
	const auto rootId = topic ? topic : MsgId(Data::ForumTopic::kGeneralId);
	if (const auto found = forum->topicFor(rootId)) {
		callback(found);
		return;
	}
	forum->requestTopic(rootId, crl::guard(this, [=] {
		const auto now = peer->forum();
		if (const auto found = now ? now->topicFor(rootId) : nullptr) {
			callback(found);
		} else {
			done(Error(u"There is no topic "_q
				+ QString::number(rootId.bare)
				+ u" in this chat; see list_topics."_q));
		}
	}));
}

void Tools::waitSent(
		not_null<Main::Session*> session,
		FullMsgId localId,
		QJsonObject chat,
		Done done) {
	auto pending = std::make_unique<PendingSend>();
	pending->session = base::make_weak(session);
	pending->localId = localId;
	pending->chat = std::move(chat);
	pending->done = std::move(done);
	pending->started = crl::now();
	const auto raw = pending.get();
	session->data().itemIdChanged(
	) | rpl::filter([=](const Data::Session::IdChange &change) {
		return (change.newId.peer == localId.peer)
			&& (change.oldId == localId.msg);
	}) | rpl::on_next([=](const Data::Session::IdChange &change) {
		auto result = QJsonObject{
			{ u"chat"_q, raw->chat },
			{ u"message_id"_q, qint64(change.newId.msg.bare) },
		};
		finishSend(raw, { .data = result });
	}, raw->lifetime);
	_sends.emplace(++_lastSendId, std::move(pending));
	if (!_sendsTimer.isActive()) {
		_sendsTimer.callEach(kSendCheckInterval);
	}
}

void Tools::finishSend(not_null<PendingSend*> pending, ToolResult result) {
	if (pending->finished) {
		return;
	}
	pending->finished = true;
	pending->done(std::move(result));
}

void Tools::checkSends() {
	// Finished ones are only dropped here, never from inside their own
	// subscription.
	const auto now = crl::now();
	for (auto i = _sends.begin(); i != _sends.end();) {
		const auto raw = i->second.get();
		if (!raw->finished) {
			const auto session = raw->session.get();
			const auto item = session
				? session->data().message(raw->localId)
				: nullptr;
			if (!session) {
				finishSend(raw, Error(u"The account was logged out."_q));
			} else if (!item) {
				finishSend(raw, Error(u"The message was not sent."_q));
			} else if (item->hasFailed()) {
				finishSend(raw, Error(u"Telegram did not accept the "
					"message; it is marked as failed in the chat."_q));
			} else if (now - raw->started >= kSendTimeout) {
				finishSend(raw, { .data = QJsonObject{
					{ u"chat"_q, raw->chat },
					{ u"status"_q, u"sending"_q },
					{ u"note"_q, u"Still sending; it will go out once the "
						"connection allows."_q },
				} });
			}
		}
		if (raw->finished) {
			i = _sends.erase(i);
		} else {
			++i;
		}
	}
	if (_sends.empty()) {
		_sendsTimer.cancel();
	}
}

void Tools::listAccounts(const QJsonObject &arguments, Done done) {
	auto &domain = Core::App().domain();
	if (!domain.started()) {
		done(Error(u"Telegram is still starting."_q));
		return;
	}
	const auto active = &domain.active();
	auto list = QJsonArray();
	for (const auto &[index, account] : domain.accounts()) {
		auto entry = QJsonObject{
			{ u"account"_q, index },
			{ u"active"_q, account.get() == active },
			{ u"logged_in"_q, account->sessionExists() },
		};
		if (account->sessionExists()) {
			const auto user = account->session().user();
			entry[u"id"_q] = BotApiId(user->id);
			entry[u"name"_q] = user->name();
			if (const auto username = user->username(); !username.isEmpty()) {
				entry[u"username"_q] = username;
			}
		}
		list.push_back(entry);
	}
	done({ .data = QJsonObject{ { u"accounts"_q, list } } });
}

void Tools::listChats(const QJsonObject &arguments, Done done) {
	auto error = QString();
	const auto target = resolveAccount(arguments, &error);
	if (!target) {
		done(Error(error));
		return;
	}
	const auto session = target->session;
	const auto archive = (StringArgument(arguments, u"folder"_q)
		== u"archive"_q);
	const auto folder = archive
		? session->data().folderLoaded(Data::Folder::kId)
		: nullptr;
	if (archive && !folder) {
		done({ .data = QJsonObject{
			{ u"account"_q, target->account },
			{ u"chats"_q, QJsonArray() },
			{ u"total"_q, 0 },
			{ u"complete"_q, true },
		} });
		return;
	}
	const auto list = session->data().chatsList(folder);
	const auto limit = LimitArgument(
		arguments,
		u"limit"_q,
		kDefaultLimit,
		kMaxLimit);
	const auto offset = std::max(
		IntArgument(arguments, u"offset"_q).value_or(0),
		qint64(0));
	const auto unreadOnly = BoolArgument(arguments, u"unread_only"_q, false);
	const auto query = StringArgument(arguments, u"query"_q).trimmed();

	auto chats = QJsonArray();
	auto matched = 0;
	for (const auto &row : list->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		if (unreadOnly
			&& !Unread(history).messages
			&& !history->unreadMark()) {
			continue;
		} else if (!query.isEmpty()
			&& !peer->name().contains(query, Qt::CaseInsensitive)
			&& !peer->username().contains(query, Qt::CaseInsensitive)) {
			continue;
		}
		if (matched++ < offset || chats.size() >= limit) {
			continue;
		}
		chats.push_back(ChatEntry(history));
	}
	auto result = QJsonObject{
		{ u"account"_q, target->account },
		{ u"chats"_q, chats },
		{ u"total"_q, matched },
		{ u"complete"_q, list->loaded() },
	};
	if (!list->loaded()) {
		result[u"note"_q] = u"The client has not loaded its whole chat "
			"list yet. A chat that is missing here can still be opened by "
			"@username or found with search_messages."_q;
	}
	done({ .data = result });
}

void Tools::getChat(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		auto result = PeerJson(peer);
		result[u"account"_q] = target.account;
		if (!peer->about().isEmpty()) {
			result[u"about"_q] = peer->about();
		}
		if (const auto channel = peer->asChannel()) {
			if (channel->membersCount() > 0) {
				result[u"members"_q] = channel->membersCount();
			}
			result[u"creator"_q] = channel->amCreator();
			result[u"admin"_q] = channel->amCreator()
				|| channel->hasAdminRights();
		} else if (const auto chat = peer->asChat()) {
			if (chat->count > 0) {
				result[u"members"_q] = chat->count;
			}
			result[u"creator"_q] = chat->amCreator();
			result[u"admin"_q] = chat->amCreator() || chat->hasAdminRights();
		}
		result[u"forum"_q] = peer->isForum();
		result[u"can_send"_q] = Data::CanSendTexts(peer, false);
		if (const auto history = peer->owner().historyLoaded(peer)) {
			const auto state = Unread(history);
			result[u"unread"_q] = state.messages;
			if (const auto mentions = history->unreadMentions().count(0)) {
				result[u"mentions"_q] = mentions;
			}
			result[u"muted"_q] = history->muted();
			if (const auto last = history->lastMessage()) {
				result[u"last_message"_q] = ItemPreview(last);
			}
		}
		if (peer->isForum()) {
			result[u"hint"_q] = u"This group is a forum: list_topics shows "
				"its topics, and read_history, send_message, set_draft and "
				"subscribe take a topic."_q;
		}
		done({ .data = result });
	});
}

void Tools::listTopics(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		if (!peer->isForum()) {
			done(Error(u"This chat has no topics."_q));
			return;
		}
		const auto session = target.session;
		const auto query = StringArgument(arguments, u"query"_q).trimmed();
		const auto limit = LimitArgument(
			arguments,
			u"limit"_q,
			kDefaultLimit,
			kMaxLimit);
		const auto offset = StringArgument(arguments, u"offset"_q).split(':');
		const auto offsetValue = [&](int index) {
			return (offset.size() == 3) ? offset[index].toInt() : 0;
		};
		using Flag = MTPmessages_GetForumTopics::Flag;
		session->api().request(MTPmessages_GetForumTopics(
			MTP_flags(query.isEmpty() ? Flag(0) : Flag::f_q),
			peer->input(),
			MTP_string(query),
			MTP_int(offsetValue(0)),
			MTP_int(offsetValue(1)),
			MTP_int(offsetValue(2)),
			MTP_int(limit)
		)).done([=](const MTPmessages_ForumTopics &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());

			auto lastMessages = base::flat_map<MsgId, QJsonObject>();
			for (const auto &message : data.vmessages().v) {
				auto json = MessageJson(session, message, true);
				if (!json.isEmpty()) {
					const auto id = MsgId(json[u"id"_q].toInteger());
					json[u"text"_q] = Preview(json[u"text"_q].toString());
					lastMessages.emplace(id, std::move(json));
				}
			}
			auto topics = QJsonArray();
			auto next = QString();
			for (const auto &topic : data.vtopics().v) {
				topic.match([&](const MTPDforumTopic &fields) {
					const auto id = fields.vid().v;
					auto entry = QJsonObject{
						{ u"id"_q, id },
						{ u"title"_q, qs(fields.vtitle()) },
						{ u"created"_q, fields.vdate().v },
						{ u"creator"_q, PeerRef(
							session,
							peerFromMTP(fields.vfrom_id())) },
						{ u"unread"_q, fields.vunread_count().v },
					};
					if (id == Data::ForumTopic::kGeneralId) {
						entry[u"general"_q] = true;
					}
					if (fields.vunread_mentions_count().v) {
						entry[u"mentions"_q]
							= fields.vunread_mentions_count().v;
					}
					if (fields.is_closed()) {
						entry[u"closed"_q] = true;
					}
					if (fields.is_pinned()) {
						entry[u"pinned"_q] = true;
					}
					if (fields.is_hidden()) {
						entry[u"hidden"_q] = true;
					}
					const auto top = MsgId(fields.vtop_message().v);
					const auto i = lastMessages.find(top);
					auto lastDate = 0;
					if (i != lastMessages.end()) {
						lastDate = i->second[u"date"_q].toInt();
						entry[u"last_message"_q] = i->second;
					}
					topics.push_back(entry);
					next = u"%1:%2:%3"_q.arg(lastDate).arg(top.bare).arg(id);
				}, [](const MTPDforumTopicDeleted &) {
				});
			}
			auto json = QJsonObject{
				{ u"chat"_q, PeerJson(peer) },
				{ u"count"_q, data.vcount().v },
				{ u"topics"_q, topics },
			};
			if (topics.size() >= limit && !next.isEmpty()) {
				json[u"next_offset"_q] = next;
			}
			done({ .data = json });
		}).fail([=](const MTP::Error &error) {
			done(Error(ApiError(error)));
		}).send();
	});
}

void Tools::readHistory(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto topic = IdArgument(arguments, u"topic"_q);
		if (topic && !peer->isForum()) {
			done(Error(u"This chat has no topics."_q));
			return;
		}
		const auto session = target.session;
		const auto limit = LimitArgument(
			arguments,
			u"limit"_q,
			kDefaultLimit,
			kMaxLimit);
		const auto beforeId = IdArgument(arguments, u"before_id"_q);
		const auto afterId = IdArgument(arguments, u"after_id"_q);

		// Newer than after_id means paging forward from it: start at it,
		// shift the window back by its size and drop it with min_id.
		const auto offsetId = afterId ? afterId : beforeId;
		const auto addOffset = afterId ? -limit : 0;
		const auto minId = afterId;
		const auto handle = [=](const MTPmessages_Messages &result) {
			const auto parsed = ParseMessages(session, result, false);
			auto json = QJsonObject{
				{ u"chat"_q, PeerJson(peer) },
				{ u"messages"_q, parsed.messages },
			};
			if (topic) {
				json[u"topic"_q] = qint64(topic.bare);
			}
			if (parsed.count) {
				json[u"total"_q] = *parsed.count;
			}
			if (parsed.messages.size() >= limit && parsed.lastId) {
				json[u"next_before_id"_q] = qint64(parsed.lastId.bare);
			}
			done({ .data = json });
		};
		const auto fail = [=](const MTP::Error &error) {
			done(Error(ApiError(error)));
		};
		if (topic) {
			session->api().request(MTPmessages_GetReplies(
				peer->input(),
				MTP_int(topic),
				MTP_int(offsetId),
				MTP_int(0), // offset_date
				MTP_int(addOffset),
				MTP_int(limit),
				MTP_int(0), // max_id
				MTP_int(minId),
				MTP_long(0) // hash
			)).done(handle).fail(fail).send();
		} else {
			session->api().request(MTPmessages_GetHistory(
				peer->input(),
				MTP_int(offsetId),
				MTP_int(0), // offset_date
				MTP_int(addOffset),
				MTP_int(limit),
				MTP_int(0), // max_id
				MTP_int(minId),
				MTP_long(0) // hash
			)).done(handle).fail(fail).send();
		}
	});
}

void Tools::searchMessages(const QJsonObject &arguments, Done done) {
	const auto query = StringArgument(arguments, u"query"_q).trimmed();
	if (query.isEmpty()) {
		done(Error(u"Pass something to search for."_q));
		return;
	}
	const auto limit = LimitArgument(
		arguments,
		u"limit"_q,
		kDefaultLimit,
		kMaxLimit);
	const auto offset = StringArgument(arguments, u"offset"_q).split(':');
	const auto fail = [=](const MTP::Error &error) {
		done(Error(ApiError(error)));
	};

	if (!arguments.contains(u"chat"_q)) {
		if (arguments.contains(u"topic"_q) || arguments.contains(u"from"_q)) {
			done(Error(u"topic and from need a chat."_q));
			return;
		}
		auto error = QString();
		const auto target = resolveAccount(arguments, &error);
		if (!target) {
			done(Error(error));
			return;
		}
		const auto session = target->session;
		const auto offsetRate = (offset.size() == 3) ? offset[0].toInt() : 0;
		const auto offsetPeerId = (offset.size() == 3)
			? PeerFromBotApi(offset[1].toLongLong())
			: std::nullopt;
		const auto offsetPeer = offsetPeerId
			? session->data().peerLoaded(*offsetPeerId)
			: nullptr;
		const auto offsetId = offsetPeer ? offset[2].toInt() : 0;
		session->api().request(MTPmessages_SearchGlobal(
			MTP_flags(0),
			MTP_int(0), // folder_id
			MTPInputChannel(), // community
			MTP_string(query),
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(offsetPeer ? offsetRate : 0),
			offsetPeer ? offsetPeer->input() : MTP_inputPeerEmpty(),
			MTP_int(offsetId),
			MTP_int(limit)
		)).done([=](const MTPmessages_Messages &result) {
			const auto parsed = ParseMessages(session, result, true);
			auto json = QJsonObject{
				{ u"account"_q, target->account },
				{ u"messages"_q, parsed.messages },
			};
			if (parsed.count) {
				json[u"total"_q] = *parsed.count;
			}
			if (parsed.messages.size() >= limit && parsed.lastId) {
				json[u"next_offset"_q] = u"%1:%2:%3"_q
					.arg(parsed.nextRate)
					.arg(BotApiId(parsed.lastPeer))
					.arg(parsed.lastId.bare);
			}
			done({ .data = json });
		}).fail(fail).send();
		return;
	}

	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto topic = IdArgument(arguments, u"topic"_q);
		if (topic && !peer->isForum()) {
			done(Error(u"This chat has no topics."_q));
			return;
		}
		const auto session = target.session;
		const auto offsetId = offset.isEmpty() ? 0 : offset[0].toInt();
		const auto search = [=](PeerData *from) {
			using Flag = MTPmessages_Search::Flag;
			session->api().request(MTPmessages_Search(
				MTP_flags((topic ? Flag::f_top_msg_id : Flag(0))
					| (from ? Flag::f_from_id : Flag(0))),
				peer->input(),
				MTP_string(query),
				from ? from->input() : MTP_inputPeerEmpty(),
				MTP_inputPeerEmpty(), // saved_peer_id
				MTP_vector<MTPReaction>(), // saved_reaction
				MTP_int(topic),
				MTP_inputMessagesFilterEmpty(),
				MTP_int(0), // min_date
				MTP_int(0), // max_date
				MTP_int(offsetId),
				MTP_int(0), // add_offset
				MTP_int(limit),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done([=](const MTPmessages_Messages &result) {
				const auto parsed = ParseMessages(session, result, false);
				auto json = QJsonObject{
					{ u"chat"_q, PeerJson(peer) },
					{ u"messages"_q, parsed.messages },
				};
				if (parsed.count) {
					json[u"total"_q] = *parsed.count;
				}
				if (parsed.messages.size() >= limit && parsed.lastId) {
					json[u"next_offset"_q] = QString::number(
						parsed.lastId.bare);
				}
				done({ .data = json });
			}).fail(fail).send();
		};
		if (!arguments.contains(u"from"_q)) {
			search(nullptr);
			return;
		}
		resolvePeer(session, arguments.value(u"from"_q), crl::guard(this, [=](
				PeerData *from,
				QString error) {
			if (!from) {
				done(Error(error));
			} else {
				search(from);
			}
		}));
	});
}

void Tools::messageVersions(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto id = IdArgument(arguments, u"message_id"_q);
		if (!id) {
			done(Error(u"Pass a message_id."_q));
			return;
		}
		const auto session = target.session;
		auto &versions = session->data().messageVersions();
		auto list = QJsonArray();
		if (const auto found = versions.lookup(FullMsgId(peer->id, id))) {
			for (const auto &version : *found) {
				auto entry = QJsonObject{
					{ u"text"_q, version.text },
					{ u"captured_at"_q, version.capturedAt },
					{ u"from"_q, PeerRef(session, version.authorId) },
				};
				if (version.deleted) {
					entry[u"deleted"_q] = true;
				}
				list.push_back(entry);
			}
		}
		auto result = QJsonObject{
			{ u"chat"_q, PeerJson(peer) },
			{ u"message_id"_q, qint64(id.bare) },
			{ u"versions"_q, list },
		};
		if (list.isEmpty() && !Core::App().settings().saveMessageVersions()) {
			result[u"note"_q] = u"Saving message versions is off in "
				"Settings > Modifications."_q;
		}
		done({ .data = result });
	});
}

void Tools::setDraft(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto topic = IdArgument(arguments, u"topic"_q);
		const auto text = StringArgument(arguments, u"text"_q);
		const auto replyTo = IdArgument(arguments, u"reply_to"_q);
		const auto session = target.session;
		withThread(peer, topic, done, [=](not_null<Data::Thread*> thread) {
			const auto history = thread->owningHistory();
			const auto topicRootId = thread->topicRootId();
			if (text.isEmpty()) {
				history->clearLocalDraft(topicRootId, PeerId());
			} else {
				const auto cursor = MessageCursor{
					int(text.size()),
					int(text.size()),
					Ui::kQFixedMax,
				};
				history->setLocalDraft(std::make_unique<Data::Draft>(
					TextWithTags{ text },
					FullReplyTo{
						.messageId = (replyTo
							? FullMsgId(peer->id, replyTo)
							: FullMsgId()),
						.topicRootId = topicRootId,
					},
					SuggestOptions(),
					cursor,
					Data::WebPageDraft()));
			}
			history->clearLocalEditDraft(topicRootId, PeerId());
			session->changes().entryUpdated(
				thread,
				Data::EntryUpdate::Flag::LocalDraftSet);
			session->local().writeDrafts(history);
			session->api().saveDraftToCloudDelayed(thread);
			auto result = QJsonObject{
				{ u"chat"_q, PeerJson(peer) },
				{ u"cleared"_q, text.isEmpty() },
			};
			if (topicRootId) {
				result[u"topic"_q] = qint64(topicRootId.bare);
			}
			done({ .data = result });
		});
	});
}

void Tools::sendMessage(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto text = StringArgument(arguments, u"text"_q);
		if (text.trimmed().isEmpty()) {
			done(Error(u"The message is empty."_q));
			return;
		}
		const auto topic = IdArgument(arguments, u"topic"_q);
		const auto replyTo = IdArgument(arguments, u"reply_to"_q);
		const auto silent = BoolArgument(arguments, u"silent"_q, false);
		const auto session = target.session;
		withThread(peer, topic, done, [=](not_null<Data::Thread*> thread) {
			if (!Data::CanSendTexts(thread)) {
				done(Error(u"You can't write here."_q));
				return;
			}
			auto action = Api::SendAction(thread);
			action.clearDraft = false;
			action.options.silent = silent;
			if (replyTo) {
				action.replyTo.messageId = FullMsgId(peer->id, replyTo);
			}
			auto message = Api::MessageToSend(action);
			message.textWithTags = TextWithTags{ text };

			auto chat = PeerJson(peer);
			if (const auto rootId = thread->topicRootId()) {
				chat[u"topic"_q] = qint64(rootId.bare);
			}
			const auto localId = session->data().nextLocalMessageId();
			waitSent(
				session,
				FullMsgId(peer->id, localId),
				std::move(chat),
				done);
			session->api().sendMessage(std::move(message), localId);
		});
	});
}

void Tools::createTopic(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto title = StringArgument(arguments, u"title"_q).trimmed();
		if (title.isEmpty()) {
			done(Error(u"The title is empty."_q));
			return;
		} else if (!peer->isForum()) {
			done(Error(u"This chat has no topics."_q));
			return;
		} else if (!peer->canCreateTopics()) {
			done(Error(u"You can't create topics here."_q));
			return;
		}
		const auto session = target.session;
		const auto randomId = base::RandomValue<uint64>();
		const auto &colors = Data::ForumTopicColorIds();
		const auto colorId = colors[base::RandomIndex(int(colors.size()))];
		using Flag = MTPmessages_CreateForumTopic::Flag;
		session->api().request(MTPmessages_CreateForumTopic(
			MTP_flags(Flag::f_icon_color),
			peer->input(),
			MTP_string(title),
			MTP_int(colorId),
			MTP_long(0), // icon_emoji_id
			MTP_long(randomId),
			MTPInputPeer() // send_as
		)).done([=](const MTPUpdates &result) {
			const auto rootId = SentMessageId(result, randomId);
			session->api().applyUpdates(result, randomId);
			auto json = QJsonObject{
				{ u"chat"_q, PeerJson(peer) },
				{ u"title"_q, title },
			};
			if (rootId) {
				json[u"topic"_q] = qint64(rootId.bare);
			}
			done({ .data = json });
		}).fail([=](const MTP::Error &error) {
			done(Error(ApiError(error)));
		}).send();
	});
}

void Tools::markRead(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto topic = IdArgument(arguments, u"topic"_q);
		const auto session = target.session;
		withThread(peer, topic, done, [=](not_null<Data::Thread*> thread) {
			auto result = QJsonObject{ { u"chat"_q, PeerJson(peer) } };
			if (const auto forumTopic = thread->asTopic()) {
				forumTopic->readTillEnd();
				result[u"topic"_q] = qint64(forumTopic->rootId().bare);
				done({ .data = result });
				return;
			}
			const auto history = thread->owningHistory();
			const auto last = history->lastServerMessage();
			if (!last) {
				result[u"note"_q] = u"Nothing loaded to mark as read."_q;
				done({ .data = result });
				return;
			}
			// The usual call moves the local state and, unless ghost mode
			// holds it back, tells the server; the second one makes sure
			// the server hears it either way.
			auto &histories = session->data().histories();
			histories.readInboxTill(last);
			histories.readInboxTillNow(last);
			result[u"read_till"_q] = qint64(last->id.bare);
			done({ .data = result });
		});
	});
}

void Tools::editMessage(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto id = IdArgument(arguments, u"message_id"_q);
		const auto text = StringArgument(arguments, u"text"_q);
		if (!id) {
			done(Error(u"Pass a message_id."_q));
			return;
		} else if (text.trimmed().isEmpty()) {
			done(Error(u"The new text is empty."_q));
			return;
		}
		const auto session = target.session;
		const auto apply = [=] {
			const auto item = session->data().message(peer->id, id);
			if (!item) {
				done(Error(u"There is no such message in this chat."_q));
				return;
			} else if (!item->allowsEdit(base::unixtime::now())) {
				done(Error(u"This message can't be edited."_q));
				return;
			}
			auto sending = TextWithEntities{ text };
			TextUtilities::PrepareForSending(
				sending,
				Ui::ItemTextOptions(item->history(), session->user()).flags);
			TextUtilities::Trim(sending);
			Api::EditTextMessage(
				item,
				sending,
				Data::WebPageDraft(),
				Api::SendOptions(),
				[=](mtpRequestId) {
					done({ .data = QJsonObject{
						{ u"chat"_q, PeerJson(peer) },
						{ u"message_id"_q, qint64(id.bare) },
						{ u"edited"_q, true },
					} });
				},
				[=](const QString &error, mtpRequestId) {
					done(Error(u"Telegram refused the edit: "_q + error));
				},
				false);
		};
		if (session->data().message(peer->id, id)) {
			apply();
		} else {
			session->api().requestMessageData(
				peer,
				id,
				crl::guard(this, apply));
		}
	});
}

void Tools::deleteMessages(const QJsonObject &arguments, Done done) {
	withPeer(arguments, done, [=](Target target, not_null<PeerData*> peer) {
		const auto ids = arguments.value(u"message_ids"_q).toArray();
		if (ids.isEmpty()) {
			done(Error(u"Pass message_ids."_q));
			return;
		} else if (ids.size() > kMaxDeleteIds) {
			done(Error(u"At most 100 messages at once."_q));
			return;
		}
		const auto revoke = BoolArgument(arguments, u"revoke"_q, true);
		const auto session = target.session;
		auto loaded = MessageIdsList();
		auto remote = QVector<MTPint>();
		auto refused = QJsonArray();
		for (const auto &value : ids) {
			const auto raw = value.isDouble()
				? qint64(value.toDouble())
				: value.toString().toLongLong();
			if (raw <= 0 || raw > std::numeric_limits<int32>::max()) {
				done(Error(u"Message ids are positive numbers."_q));
				return;
			}
			const auto itemId = FullMsgId(peer->id, MsgId(raw));
			if (const auto item = session->data().message(itemId)) {
				if (!item->canDelete()) {
					refused.push_back(raw);
				} else {
					loaded.push_back(itemId);
				}
			} else {
				remote.push_back(MTP_int(raw));
			}
		}
		auto &histories = session->data().histories();
		if (!loaded.empty()) {
			// Removes them from the screen right away, as the box does.
			histories.deleteMessages(loaded, revoke);
		}
		if (!remote.isEmpty()) {
			histories.deleteMessages(
				session->data().history(peer),
				remote,
				revoke);
		}
		session->data().sendHistoryChangeNotifications();
		auto result = QJsonObject{
			{ u"chat"_q, PeerJson(peer) },
			{ u"requested"_q, int(loaded.size() + remote.size()) },
			{ u"revoke"_q, revoke },
		};
		if (!refused.isEmpty()) {
			result[u"not_allowed"_q] = refused;
		}
		done({ .data = result });
	});
}

void Tools::getClientSettings(const QJsonObject &arguments, Done done) {
	auto list = QJsonArray();
	for (const auto &setting : ClientSettings()) {
		list.push_back(QJsonObject{
			{ u"name"_q, setting.name },
			{ u"value"_q, setting.get() },
			{ u"description"_q, setting.description },
		});
	}
	done({ .data = QJsonObject{ { u"settings"_q, list } } });
}

void Tools::setClientSetting(const QJsonObject &arguments, Done done) {
	const auto name = StringArgument(arguments, u"name"_q);
	const auto value = arguments.value(u"value"_q);
	if (!value.isBool()) {
		done(Error(u"value must be true or false."_q));
		return;
	}
	for (const auto &setting : ClientSettings()) {
		if (setting.name == name) {
			setting.set(value.toBool());
			Core::App().saveSettingsDelayed();
			done({ .data = QJsonObject{
				{ u"name"_q, name },
				{ u"value"_q, setting.get() },
			} });
			return;
		}
	}
	done(Error(u"There is no setting \""_q
		+ name
		+ u"\"; see get_client_settings."_q));
}

void Tools::subscribe(const QJsonObject &arguments, Done done) {
	auto types = EventTypes();
	const auto events = arguments.value(u"events"_q);
	if (events.isArray()) {
		for (const auto &value : events.toArray()) {
			const auto type = EventTypeFromName(value.toString());
			if (!type) {
				done(Error(u"Unknown event \""_q
					+ value.toString()
					+ u"\"; use message, mention, topic_created or "
					"edited."_q));
				return;
			}
			types |= *type;
		}
	}
	if (!types) {
		types = EventType::Message | EventType::TopicCreated;
	}
	auto subscription = Subscription{
		.types = types,
		.includeOutgoing = BoolArgument(
			arguments,
			u"include_outgoing"_q,
			false),
	};
	const auto topic = IdArgument(arguments, u"topic"_q);
	const auto hasChat = arguments.contains(u"chat"_q);
	const auto hasFrom = arguments.contains(u"from"_q);
	if (topic && !hasChat) {
		done(Error(u"topic needs a chat."_q));
		return;
	}
	const auto finish = [=](Subscription subscription) {
		subscription.id = _events->subscribe(subscription);
		done({ .data = QJsonObject{
			{ u"subscription"_q, SubscriptionJson(subscription) },
		} });
	};
	if (!hasChat && !hasFrom && !arguments.contains(u"account"_q)) {
		finish(subscription);
		return;
	}
	auto error = QString();
	const auto target = resolveAccount(arguments, &error);
	if (!target) {
		done(Error(error));
		return;
	}
	subscription.account = target->account;
	const auto session = target->session;
	const auto withFrom = [=](Subscription subscription) {
		if (!hasFrom) {
			finish(subscription);
			return;
		}
		resolvePeer(session, arguments.value(u"from"_q), crl::guard(this, [=](
				PeerData *from,
				QString error) {
			if (!from) {
				done(Error(error));
				return;
			}
			auto copy = subscription;
			copy.from = from->id;
			finish(copy);
		}));
	};
	if (!hasChat) {
		withFrom(subscription);
		return;
	}
	resolvePeer(session, arguments.value(u"chat"_q), crl::guard(this, [=](
			PeerData *peer,
			QString error) {
		if (!peer) {
			done(Error(error));
			return;
		} else if (topic && !peer->isForum()) {
			done(Error(u"This chat has no topics."_q));
			return;
		}
		auto copy = subscription;
		copy.peer = peer->id;
		copy.topic = topic;
		withFrom(copy);
	}));
}

void Tools::unsubscribe(const QJsonObject &arguments, Done done) {
	if (BoolArgument(arguments, u"all"_q, false)) {
		const auto count = int(_events->subscriptions().size());
		_events->clear();
		done({ .data = QJsonObject{ { u"removed"_q, count } } });
		return;
	}
	const auto id = IntArgument(arguments, u"id"_q);
	if (!id) {
		done(Error(u"Pass a subscription id, or all."_q));
	} else if (!_events->unsubscribe(int(*id))) {
		done(Error(u"There is no such subscription."_q));
	} else {
		done({ .data = QJsonObject{ { u"removed"_q, 1 } } });
	}
}

void Tools::listSubscriptions(const QJsonObject &arguments, Done done) {
	auto list = QJsonArray();
	for (const auto &subscription : _events->subscriptions()) {
		list.push_back(SubscriptionJson(subscription));
	}
	done({ .data = QJsonObject{ { u"subscriptions"_q, list } } });
}

void Tools::pollEvents(const QJsonObject &arguments, Done done) {
	const auto max = LimitArgument(
		arguments,
		u"max"_q,
		kDefaultPollEvents,
		kMaxPollEvents);
	const auto seconds = std::clamp(
		IntArgument(arguments, u"timeout_seconds"_q).value_or(0),
		qint64(0),
		qint64(kMaxPollSeconds));
	_events->poll(max, seconds * crl::time(1000), [=](QJsonArray events) {
		auto result = QJsonObject{ { u"events"_q, events } };
		if (events.isEmpty() && _events->subscriptions().empty()) {
			result[u"note"_q] = u"Nothing is subscribed; see subscribe."_q;
		}
		done({ .data = result });
	});
}

} // namespace Mcp
