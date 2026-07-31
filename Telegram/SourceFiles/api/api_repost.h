/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Data {
class Thread;
} // namespace Data

namespace Window {
class SessionNavigation;
} // namespace Window

namespace Api {

struct MessageToSend;

// A message the server has deleted has no id left to reply to, and neither do
// the local copies we restore after a restart. Rather than let the send fail
// with MESSAGE_ID_INVALID, fold the original into the outgoing text as a
// quote and send an ordinary message.
void RewriteReplyToDeletedAsQuote(MessageToSend &message);

// True if there is anything we can rebuild a message out of.
[[nodiscard]] bool CanRepost(not_null<HistoryItem*> item);

// Sends the contents of `item` into `thread` as a brand new message of ours,
// quoting the original author. Unlike a forward this is an ordinary send, so
// it works where forwarding is refused: chats that restrict saving content,
// and the local copies we keep of deleted messages, which have no server id
// to forward in the first place.
void RepostMessageTo(
	not_null<Data::Thread*> thread,
	not_null<HistoryItem*> item);

// Asks for a recipient, then reposts into it.
void RepostMessage(
	not_null<Window::SessionNavigation*> navigation,
	not_null<HistoryItem*> item);

} // namespace Api
