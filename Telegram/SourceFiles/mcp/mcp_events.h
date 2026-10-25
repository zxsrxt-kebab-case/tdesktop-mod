/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"
#include "base/weak_ptr.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Mcp {

enum class EventType : uint32 {
	Message      = (1U << 0),
	Mention      = (1U << 1),
	TopicCreated = (1U << 2),
	Edited       = (1U << 3),
};
inline constexpr bool is_flag_type(EventType) { return true; }
using EventTypes = base::flags<EventType>;

// What a client asked to hear about. Every filter left at zero matches
// anything, so an empty subscription follows every chat of every account.
struct Subscription {
	int id = 0;
	int account = -1;
	PeerId peer = 0;
	MsgId topic = 0;
	PeerId from = 0;
	EventTypes types;
	bool includeOutgoing = false;
};

[[nodiscard]] QString EventTypeName(EventType type);
[[nodiscard]] std::optional<EventType> EventTypeFromName(const QString &name);

// Turns what happens in the sessions into events for the subscriptions.
//
// They go out two ways: pushed to the open event streams, which is what a
// Claude Code channel listens to, and queued for poll_events, for clients
// that cannot keep a stream. Nothing is collected while the server is down
// or there is no subscription, so idle costs nothing but one comparison per
// new message.
class Events final : public base::has_weak_ptr {
public:
	Events();
	~Events();

	void setActive(bool active);

	[[nodiscard]] int subscribe(Subscription subscription);
	bool unsubscribe(int id);
	void clear();
	[[nodiscard]] const std::vector<Subscription> &subscriptions() const;

	[[nodiscard]] rpl::producer<QJsonObject> pushed() const;
	void poll(int max, crl::time timeout, Fn<void(QJsonArray)> done);

private:
	struct Waiter {
		uint64 id = 0;
		int max = 0;
		Fn<void(QJsonArray)> done;
	};

	void watchAccounts();
	void watchSession(
		int account,
		not_null<Main::Session*> session,
		rpl::lifetime &lifetime);
	void itemAdded(int account, not_null<HistoryItem*> item);
	void itemEdited(int account, not_null<HistoryItem*> item);
	void push(QJsonObject event);
	void feedWaiters();
	[[nodiscard]] QJsonArray take(int max);

	std::vector<Subscription> _subscriptions;
	int _lastSubscriptionId = 0;
	std::deque<QJsonObject> _queue;
	std::vector<Waiter> _waiters;
	uint64 _lastWaiterId = 0;
	base::flat_map<int, rpl::lifetime> _accounts;
	base::flat_map<int, rpl::lifetime> _sessions;
	rpl::event_stream<QJsonObject> _pushed;
	bool _active = false;
	rpl::lifetime _lifetime;

};

} // namespace Mcp
