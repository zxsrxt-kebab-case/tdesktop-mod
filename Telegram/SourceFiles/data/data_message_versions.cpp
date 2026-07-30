/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_message_versions.h"

#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/text/text_entity.h"

#include <QtCore/QBuffer>
#include <QtCore/QDataStream>

namespace Data {
namespace {

constexpr auto kSaveDelay = crl::time(5000);
constexpr auto kSerializeVersion = qint32(1);

// A single blob is rewritten whole on every save, so it has to stay small
// enough that saving is cheap. Oldest entries are dropped past these.
constexpr auto kMaxMessages = 20000;
constexpr auto kMaxVersionsPerMessage = 32;
constexpr auto kMaxApproximateBytes = 32 * 1024 * 1024;

[[nodiscard]] int ApproximateSize(const MessageVersion &version) {
	return sizeof(MessageVersion)
		+ (version.text.size() * sizeof(QChar))
		+ version.tags.size();
}

} // namespace

MessageVersions::MessageVersions(not_null<Session*> owner)
: _owner(owner)
, _saveTimer([=] { save(); }) {
}

MessageVersions::~MessageVersions() {
	if (_saveTimer.isActive()) {
		_saveTimer.cancel();
		save();
	}
}

void MessageVersions::captureBeforeEdit(not_null<HistoryItem*> item) {
	capture(item, false);
}

void MessageVersions::captureBeforeDelete(not_null<HistoryItem*> item) {
	capture(item, true);
}

void MessageVersions::capture(not_null<HistoryItem*> item, bool deleted) {
	if (!item->isRegular()) {
		return;
	}
	load();

	const auto original = item->originalText();
	if (original.text.isEmpty()) {
		// Nothing worth keeping - media-only messages are not stored, their
		// files live in the cache and are not ours to duplicate.
		return;
	}
	auto &list = _versions[item->fullId()];
	const auto tags = TextUtilities::SerializeTags(
		TextUtilities::ConvertEntitiesToTextTags(original.entities));
	if (!list.empty()
		&& list.back().text == original.text
		&& list.back().tags == tags
		&& list.back().deleted == deleted) {
		return; // Same content captured twice in a row.
	}
	auto version = MessageVersion{
		.authorId = item->from()->id,
		.capturedAt = base::unixtime::now(),
		.text = original.text,
		.tags = tags,
		.deleted = deleted,
	};
	DEBUG_LOG(("MessageVersions: captured %1 for %2_%3, versions now %4."
		).arg(deleted ? "delete" : "edit"
		).arg(item->fullId().peer.value
		).arg(item->fullId().msg.bare
		).arg(list.size() + 1));
	_approximateBytes += ApproximateSize(version);
	list.push_back(std::move(version));
	while (list.size() > kMaxVersionsPerMessage) {
		_approximateBytes -= ApproximateSize(list.front());
		list.erase(list.begin());
	}
	enforceLimits();
	scheduleSave();
}

void MessageVersions::enforceLimits() {
	// Drop whole messages, oldest capture first, until back within budget.
	while ((_versions.size() > kMaxMessages
			|| _approximateBytes > kMaxApproximateBytes)
		&& !_versions.empty()) {
		auto oldest = _versions.begin();
		auto oldestTime = TimeId(0);
		for (auto i = _versions.begin(); i != _versions.end(); ++i) {
			if (i->second.empty()) {
				oldest = i;
				break;
			}
			const auto time = i->second.back().capturedAt;
			if (!oldestTime || time < oldestTime) {
				oldestTime = time;
				oldest = i;
			}
		}
		for (const auto &version : oldest->second) {
			_approximateBytes -= ApproximateSize(version);
		}
		_versions.erase(oldest);
	}
	if (_approximateBytes < 0) {
		_approximateBytes = 0;
	}
}

const std::vector<MessageVersion> *MessageVersions::lookup(FullMsgId id) {
	// Without this the saved file stays unread until something new is
	// captured, so after a restart nothing would ever be found.
	load();

	const auto i = _versions.find(id);
	DEBUG_LOG(("MessageVersions: lookup %1_%2, loaded %3 messages, found %4."
		).arg(id.peer.value
		).arg(id.msg.bare
		).arg(_versions.size()
		).arg((i != _versions.end() && !i->second.empty()) ? 1 : 0));
	return (i != _versions.end() && !i->second.empty())
		? &i->second
		: nullptr;
}

bool MessageVersions::has(FullMsgId id) {
	return lookup(id) != nullptr;
}

void MessageVersions::markLocallyDeleted(FullMsgId id) {
	_locallyDeleted.emplace(id);
}

bool MessageVersions::locallyDeleted(FullMsgId id) const {
	return _locallyDeleted.contains(id);
}

void MessageVersions::clear() {
	_versions.clear();
	_locallyDeleted.clear();
	_approximateBytes = 0;
	_loaded = true;
	scheduleSave();
}

void MessageVersions::scheduleSave() {
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

void MessageVersions::load() {
	if (_loaded) {
		return;
	}
	_loaded = true;
	const auto data = _owner->session().local().readMessageVersions();
	deserialize(data);
	DEBUG_LOG(("MessageVersions: loaded %1 bytes, %2 messages."
		).arg(data.size()
		).arg(_versions.size()));
}

void MessageVersions::save() {
	if (!_loaded) {
		return;
	}
	_owner->session().local().writeMessageVersions(serialize());
}

QByteArray MessageVersions::serialize() const {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	buffer.open(QIODevice::WriteOnly);
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);

	stream << kSerializeVersion << qint32(_versions.size());
	for (const auto &[id, list] : _versions) {
		stream
			<< quint64(id.peer.value)
			<< qint32(id.msg.bare)
			<< qint32(list.size());
		for (const auto &version : list) {
			stream
				<< quint64(version.authorId.value)
				<< qint32(version.capturedAt)
				<< version.text
				<< version.tags
				<< qint32(version.deleted ? 1 : 0);
		}
	}
	buffer.close();
	return result;
}

void MessageVersions::deserialize(const QByteArray &data) {
	_versions.clear();
	_approximateBytes = 0;
	if (data.isEmpty()) {
		return;
	}
	auto buffer = QBuffer();
	buffer.setData(data);
	buffer.open(QIODevice::ReadOnly);
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);

	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if (stream.status() != QDataStream::Ok
		|| version != kSerializeVersion
		|| count < 0
		|| count > kMaxMessages) {
		LOG(("App Error: Bad message versions data."));
		return;
	}
	for (auto i = 0; i != count; ++i) {
		auto peerId = quint64();
		auto msgId = qint32();
		auto versionsCount = qint32();
		stream >> peerId >> msgId >> versionsCount;
		if (stream.status() != QDataStream::Ok
			|| versionsCount < 0
			|| versionsCount > kMaxVersionsPerMessage) {
			LOG(("App Error: Bad message versions entry."));
			_versions.clear();
			_approximateBytes = 0;
			return;
		}
		auto list = std::vector<MessageVersion>();
		list.reserve(versionsCount);
		for (auto j = 0; j != versionsCount; ++j) {
			auto entry = MessageVersion();
			auto authorId = quint64();
			auto capturedAt = qint32();
			auto deleted = qint32();
			stream
				>> authorId
				>> capturedAt
				>> entry.text
				>> entry.tags
				>> deleted;
			if (stream.status() != QDataStream::Ok) {
				LOG(("App Error: Bad message version."));
				_versions.clear();
				_approximateBytes = 0;
				return;
			}
			entry.authorId = PeerId(authorId);
			entry.capturedAt = capturedAt;
			entry.deleted = (deleted == 1);
			_approximateBytes += ApproximateSize(entry);
			list.push_back(std::move(entry));
		}
		if (!list.empty()) {
			_versions.emplace(
				FullMsgId(PeerId(peerId), MsgId(msgId)),
				std::move(list));
		}
	}
}

} // namespace Data
