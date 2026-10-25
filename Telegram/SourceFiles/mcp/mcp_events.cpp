/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mcp/mcp_events.h"

#include "base/call_delayed.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mcp/mcp_format.h"

namespace Mcp {
namespace {

// Enough to bridge a client that polls every few seconds through a busy
// group; older events are dropped first.
constexpr auto kMaxQueued = 500;

} // namespace

QString EventTypeName(EventType type) {
	switch (type) {
	case EventType::Message: return u"message"_q;
	case EventType::Mention: return u"mention"_q;
	case EventType::TopicCreated: return u"topic_created"_q;
	case EventType::Edited: return u"edited"_q;
	}
	Unexpected("Type in Mcp::EventTypeName.");
}

std::optional<EventType> EventTypeFromName(const QString &name) {
	for (const auto type : {
			EventType::Message,
			EventType::Mention,
			EventType::TopicCreated,
			EventType::Edited }) {
		if (EventTypeName(type) == name) {
			return type;
		}
	}
	return std::nullopt;
}

Events::Events() {
	watchAccounts();
}

Events::~Events() {
	for (const auto &waiter : base::take(_waiters)) {
		waiter.done(QJsonArray());
	}
}

void Events::setActive(bool active) {
	_active = active;
	if (!active) {
		_queue.clear();
		for (const auto &waiter : base::take(_waiters)) {
			waiter.done(QJsonArray());
		}
	}
}

int Events::subscribe(Subscription subscription) {
	subscription.id = ++_lastSubscriptionId;
	_subscriptions.push_back(subscription);
	return subscription.id;
}

bool Events::unsubscribe(int id) {
	const auto i = ranges::find(_subscriptions, id, &Subscription::id);
	if (i == end(_subscriptions)) {
		return false;
	}
	_subscriptions.erase(i);
	return true;
}

void Events::clear() {
	_subscriptions.clear();
	_queue.clear();
}

const std::vector<Subscription> &Events::subscriptions() const {
	return _subscriptions;
}

rpl::producer<QJsonObject> Events::pushed() const {
	return _pushed.events();
}

void Events::watchAccounts() {
	auto &domain = Core::App().domain();
	rpl::single(
		rpl::empty
	) | rpl::then(
		domain.accountsChanges()
	) | rpl::on_next([=] {
		auto alive = base::flat_set<int>();
		for (const auto &[index, account] : Core::App().domain().accounts()) {
			alive.emplace(index);
			if (_accounts.contains(index)) {
				continue;
			}
			// The session goes to nullptr before it is destroyed, so its
			// subscriptions are always dropped while it is still alive.
			account->sessionValue(
			) | rpl::on_next([=](Main::Session *session) {
				auto &lifetime = _sessions[index];
				lifetime.destroy();
				if (session) {
					watchSession(index, session, lifetime);
				}
			}, _accounts[index]);
		}
		for (auto i = _accounts.begin(); i != _accounts.end();) {
			if (alive.contains(i->first)) {
				++i;
			} else {
				_sessions.remove(i->first);
				i = _accounts.erase(i);
			}
		}
	}, _lifetime);
}

void Events::watchSession(
		int account,
		not_null<Main::Session*> session,
		rpl::lifetime &lifetime) {
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		itemAdded(account, item);
	}, lifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		itemEdited(account, update.item);
	}, lifetime);
}

void Events::itemAdded(int account, not_null<HistoryItem*> item) {
	if (!_active || _subscriptions.empty() || !item->isRegular()) {
		return;
	}
	const auto info = item->Get<HistoryServiceTopicInfo>();
	const auto topicCreated = info && info->created();
	if (item->isService() && !topicCreated) {
		// Joins, pins, title changes and the like are left to the client
		// to read on demand; they would mostly be noise in a stream.
		return;
	}
	const auto mention = !item->out() && item->mentionsMe();
	const auto type = topicCreated
		? EventType::TopicCreated
		: mention
		? EventType::Mention
		: EventType::Message;
	const auto peer = item->history()->peer;
	const auto topic = topicCreated ? item->id : item->topicRootId();

	auto matched = QJsonArray();
	for (const auto &subscription : _subscriptions) {
		if (subscription.account >= 0 && subscription.account != account) {
			continue;
		} else if (subscription.peer && subscription.peer != peer->id) {
			continue;
		} else if (subscription.topic && subscription.topic != topic) {
			continue;
		} else if (subscription.from
			&& subscription.from != item->from()->id) {
			continue;
		} else if (item->out() && !subscription.includeOutgoing) {
			continue;
		}
		const auto wanted = topicCreated
			? (subscription.types & EventType::TopicCreated)
			: mention
			? (subscription.types & (EventType::Mention | EventType::Message))
			: (subscription.types & EventType::Message);
		if (wanted) {
			matched.push_back(subscription.id);
		}
	}
	if (matched.isEmpty()) {
		return;
	}

	auto event = QJsonObject{
		{ u"event"_q, EventTypeName(type) },
		{ u"account"_q, account },
		{ u"chat"_q, PeerJson(peer) },
		{ u"message"_q, ItemJson(item) },
		{ u"subscriptions"_q, matched },
	};
	if (topicCreated) {
		event[u"topic"_q] = QJsonObject{
			{ u"id"_q, qint64(item->id.bare) },
			{ u"title"_q, info->title },
		};
	} else if (const auto forumTopic = item->topic()) {
		event[u"topic"_q] = QJsonObject{
			{ u"id"_q, qint64(forumTopic->rootId().bare) },
			{ u"title"_q, forumTopic->title() },
		};
	}
	push(std::move(event));
}

void Events::itemEdited(int account, not_null<HistoryItem*> item) {
	if (!_active || _subscriptions.empty() || !item->isRegular()) {
		return;
	} else if (item->isService()) {
		return;
	}
	const auto peer = item->history()->peer;
	const auto topic = item->topicRootId();
	auto matched = QJsonArray();
	for (const auto &subscription : _subscriptions) {
		if (!(subscription.types & EventType::Edited)) {
			continue;
		} else if (subscription.account >= 0
			&& subscription.account != account) {
			continue;
		} else if (subscription.peer && subscription.peer != peer->id) {
			continue;
		} else if (subscription.topic && subscription.topic != topic) {
			continue;
		} else if (subscription.from
			&& subscription.from != item->from()->id) {
			continue;
		} else if (item->out() && !subscription.includeOutgoing) {
			continue;
		}
		matched.push_back(subscription.id);
	}
	if (matched.isEmpty()) {
		return;
	}
	auto event = QJsonObject{
		{ u"event"_q, EventTypeName(EventType::Edited) },
		{ u"account"_q, account },
		{ u"chat"_q, PeerJson(peer) },
		{ u"message"_q, ItemJson(item) },
		{ u"subscriptions"_q, matched },
	};
	if (const auto forumTopic = item->topic()) {
		event[u"topic"_q] = QJsonObject{
			{ u"id"_q, qint64(forumTopic->rootId().bare) },
			{ u"title"_q, forumTopic->title() },
		};
	}
	push(std::move(event));
}

void Events::push(QJsonObject event) {
	_queue.push_back(event);
	while (_queue.size() > kMaxQueued) {
		_queue.pop_front();
	}
	_pushed.fire(std::move(event));
	feedWaiters();
}

QJsonArray Events::take(int max) {
	auto result = QJsonArray();
	while (!_queue.empty() && result.size() < max) {
		result.push_back(std::move(_queue.front()));
		_queue.pop_front();
	}
	return result;
}

void Events::feedWaiters() {
	while (!_waiters.empty() && !_queue.empty()) {
		auto waiter = std::move(_waiters.front());
		_waiters.erase(begin(_waiters));
		waiter.done(take(waiter.max));
	}
}

void Events::poll(int max, crl::time timeout, Fn<void(QJsonArray)> done) {
	if (!_queue.empty() || timeout <= 0 || !_active) {
		done(take(max));
		return;
	}
	const auto id = ++_lastWaiterId;
	_waiters.push_back({ .id = id, .max = max, .done = std::move(done) });
	base::call_delayed(timeout, this, [=] {
		const auto i = ranges::find(_waiters, id, &Waiter::id);
		if (i != end(_waiters)) {
			auto waiter = std::move(*i);
			_waiters.erase(i);
			waiter.done(QJsonArray());
		}
	});
}

} // namespace Mcp
