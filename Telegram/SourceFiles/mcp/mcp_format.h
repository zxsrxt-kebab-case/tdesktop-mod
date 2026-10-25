/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Mcp {

// Everything a client sees uses the Bot API form of ids, the same one the
// profile shows: a user as is, a basic group negated, a channel or a
// supergroup negated under a -100 prefix.
[[nodiscard]] qint64 BotApiId(PeerId id);
[[nodiscard]] std::optional<PeerId> PeerFromBotApi(qint64 id);

[[nodiscard]] QString PeerTypeName(not_null<PeerData*> peer);
[[nodiscard]] QJsonObject PeerJson(not_null<PeerData*> peer);
[[nodiscard]] QJsonObject PeerRef(
	not_null<Main::Session*> session,
	PeerId id);

[[nodiscard]] QString ItemText(not_null<HistoryItem*> item);
[[nodiscard]] QJsonObject ItemJson(not_null<HistoryItem*> item);

// Formats a message straight from a server response, without making it a
// part of the loaded history: reading through MCP must not change what the
// client shows, and must not count as having read anything. Empty for a
// deleted message.
[[nodiscard]] QJsonObject MessageJson(
	not_null<Main::Session*> session,
	const MTPMessage &message,
	bool forum);

} // namespace Mcp
