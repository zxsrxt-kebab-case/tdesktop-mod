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

constexpr auto kRecordVersion = qint32(2);

constexpr auto kMaxMessages = 200000;
constexpr auto kMaxVersionsPerMessage = 64;
constexpr auto kMaxApproximateBytes = 256 * 1024 * 1024;

// Rewrite the file once it holds this much more than is still live.
constexpr auto kCompactExtraRecords = 512;
constexpr auto kCompactRatio = 2;

[[nodiscard]] int ApproximateSize(const MessageVersion &version) {
	return sizeof(MessageVersion)
		+ (version.text.size() * sizeof(QChar))
		+ version.tags.size();
}

} // namespace

MessageVersions::MessageVersions(not_null<Session*> owner)
: _owner(owner) {
}

MessageVersions::~MessageVersions() = default;

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
	const auto id = item->fullId();
	auto &list = _versions[id];
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
		.originalDate = item->date(),
		.capturedAt = base::unixtime::now(),
		.text = original.text,
		.tags = tags,
		.deleted = deleted,
	};
	DEBUG_LOG(("MessageVersions: captured %1 for %2_%3, versions now %4."
		).arg(deleted ? "delete" : "edit"
		).arg(id.peer.value
		).arg(id.msg.bare
		).arg(list.size() + 1));

	// Appended before the in-memory trimming below, so that what is on disk
	// and what is in memory agree on this version existing.
	_owner->session().local().appendMessageVersion(
		serializeRecord(id, version));
	++_fileRecords;
	++_liveRecords;

	_approximateBytes += ApproximateSize(version);
	list.push_back(std::move(version));
	while (list.size() > kMaxVersionsPerMessage) {
		_approximateBytes -= ApproximateSize(list.front());
		list.erase(list.begin());
		--_liveRecords;
	}
	enforceLimits();
	compactIfNeeded();
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
			--_liveRecords;
		}
		_versions.erase(oldest);
	}
	if (_approximateBytes < 0) {
		_approximateBytes = 0;
	}
	if (_liveRecords < 0) {
		_liveRecords = 0;
	}
}

void MessageVersions::compactIfNeeded() {
	if (_fileRecords <= (_liveRecords * kCompactRatio) + kCompactExtraRecords) {
		return;
	}
	auto records = std::vector<QByteArray>();
	records.reserve(_liveRecords);
	for (const auto &[id, list] : _versions) {
		for (const auto &version : list) {
			records.push_back(serializeRecord(id, version));
		}
	}
	DEBUG_LOG(("MessageVersions: compacting %1 records down to %2."
		).arg(_fileRecords
		).arg(records.size()));
	_owner->session().local().rewriteMessageVersions(records);
	_fileRecords = int(records.size());
	_liveRecords = _fileRecords;
}

const std::vector<MessageVersion> *MessageVersions::lookup(FullMsgId id) {
	// Without this the saved file stays unread until something new is
	// captured, so after a restart nothing would ever be found.
	load();

	const auto i = _versions.find(id);
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

void MessageVersions::restoreInto(not_null<History*> history) {
	const auto peerId = history->peer->id;
	if (_restoredHistories.contains(peerId)) {
		return;
	}
	load();
	_restoredHistories.emplace(peerId);

	const auto owner = &history->owner();
	const auto selfId = history->session().userPeerId();
	for (const auto &[id, list] : _versions) {
		if (id.peer != peerId || list.empty()) {
			continue;
		}
		const auto &version = list.back();
		if (!version.deleted || owner->message(id)) {
			// Either it was only ever edited, or the server still has it.
			continue;
		}
		auto text = TextWithEntities{
			version.text,
			TextUtilities::ConvertTextTagsToEntities(
				TextUtilities::DeserializeTags(
					version.tags,
					version.text.size())),
		};
		const auto item = history->makeMessage({
			.id = owner->nextLocalMessageId(),
			.flags = (MessageFlag::HistoryEntry
				| MessageFlag::Local
				| ((version.authorId == selfId)
					? MessageFlag::Outgoing
					: MessageFlag())
				| (version.authorId
					? MessageFlag::HasFromId
					: MessageFlag())),
			.from = version.authorId,
			.date = version.originalDate,
		}, text, MTP_messageMediaEmpty());
		DEBUG_LOG(("MessageVersions: restored %1_%2, date %3."
			).arg(id.peer.value
			).arg(id.msg.bare
			).arg(version.originalDate));

		// The recreated item has a new local id, so the badge has to follow.
		markLocallyDeleted(item->fullId());
	}
}

void MessageVersions::clear() {
	_versions.clear();
	_locallyDeleted.clear();
	_restoredHistories.clear();
	_approximateBytes = 0;
	_fileRecords = 0;
	_liveRecords = 0;
	_loaded = true;
	_owner->session().local().rewriteMessageVersions({});
}

void MessageVersions::load() {
	if (_loaded) {
		return;
	}
	_loaded = true;

	const auto records = _owner->session().local().readMessageVersionRecords();
	_fileRecords = int(records.size());
	for (const auto &record : records) {
		if (!applyRecord(record)) {
			LOG(("App Error: Bad message version record, stopping."));
			break;
		}
	}
	DEBUG_LOG(("MessageVersions: loaded %1 records, %2 messages."
		).arg(_fileRecords
		).arg(_versions.size()));
	enforceLimits();
	compactIfNeeded();
}

QByteArray MessageVersions::serializeRecord(
		FullMsgId id,
		const MessageVersion &version) const {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	buffer.open(QIODevice::WriteOnly);
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);

	stream
		<< kRecordVersion
		<< quint64(id.peer.value)
		<< qint64(id.msg.bare)
		<< quint64(version.authorId.value)
		<< qint32(version.originalDate)
		<< qint32(version.capturedAt)
		<< version.text
		<< version.tags
		<< qint32(version.deleted ? 1 : 0);
	buffer.close();
	return result;
}

bool MessageVersions::applyRecord(const QByteArray &record) {
	auto buffer = QBuffer();
	buffer.setData(record);
	buffer.open(QIODevice::ReadOnly);
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);

	auto version = qint32();
	auto peerId = quint64();
	auto msgId = qint64();
	auto authorId = quint64();
	auto originalDate = qint32();
	auto capturedAt = qint32();
	auto entry = MessageVersion();
	auto deleted = qint32();
	stream
		>> version
		>> peerId
		>> msgId
		>> authorId
		>> originalDate
		>> capturedAt
		>> entry.text
		>> entry.tags
		>> deleted;
	if (stream.status() != QDataStream::Ok || version != kRecordVersion) {
		return false;
	}
	entry.authorId = PeerId(authorId);
	entry.originalDate = originalDate;
	entry.capturedAt = capturedAt;
	entry.deleted = (deleted == 1);

	_approximateBytes += ApproximateSize(entry);
	++_liveRecords;
	auto &list = _versions[FullMsgId(PeerId(peerId), MsgId(msgId))];
	list.push_back(std::move(entry));
	while (list.size() > kMaxVersionsPerMessage) {
		_approximateBytes -= ApproximateSize(list.front());
		list.erase(list.begin());
		--_liveRecords;
	}
	return true;
}

} // namespace Data
