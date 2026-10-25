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

// Reads one appended record. Used on a worker thread, so it touches nothing
// but its arguments.
[[nodiscard]] bool ParseRecord(
		const QByteArray &record,
		FullMsgId &id,
		MessageVersion &entry) {
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
	id = FullMsgId(PeerId(peerId), MsgId(msgId));
	entry.authorId = PeerId(authorId);
	entry.originalDate = originalDate;
	entry.capturedAt = capturedAt;
	entry.deleted = (deleted == 1);
	return true;
}

void TrimVersionList(
		std::vector<MessageVersion> &list,
		int &liveRecords,
		int &approximateBytes) {
	while (list.size() > kMaxVersionsPerMessage) {
		approximateBytes -= ApproximateSize(list.front());
		list.erase(list.begin());
		--liveRecords;
	}
}

// Drops whole messages, oldest capture first, until back within budget.
//
// One drop per full scan for the oldest capture was fine while the limits
// were crossed by a single capture at a time, and minutes of work the first
// time a file far over them was read. So the ages are ordered once and read
// from the oldest end, and the survivors are moved into a fresh map in one
// pass - erasing from the middle of a flat map is a memmove each time.
void TrimToLimits(
		base::flat_map<FullMsgId, std::vector<MessageVersion>> &versions,
		int &liveRecords,
		int &approximateBytes) {
	auto size = int(versions.size());
	const auto over = [&] {
		return (size > kMaxMessages)
			|| (approximateBytes > kMaxApproximateBytes);
	};
	if (!over()) {
		return;
	}
	struct AgeAndId {
		TimeId capturedAt = 0;
		FullMsgId id;
	};
	auto byAge = std::vector<AgeAndId>();
	byAge.reserve(versions.size());
	for (const auto &[id, list] : versions) {
		byAge.push_back({
			.capturedAt = (list.empty() ? TimeId(0) : list.back().capturedAt),
			.id = id,
		});
	}
	ranges::sort(byAge, ranges::less(), &AgeAndId::capturedAt);
	auto dropped = 0;
	for (const auto &entry : byAge) {
		if (!over()) {
			break;
		}
		const auto i = versions.find(entry.id);
		Assert(i != versions.end());
		for (const auto &version : i->second) {
			approximateBytes -= ApproximateSize(version);
			--liveRecords;
		}
		i->second.clear();
		--size;
		++dropped;
	}
	if (dropped) {
		auto kept = base::flat_map<FullMsgId, std::vector<MessageVersion>>();
		kept.reserve(versions.size() - dropped);
		for (auto &[id, list] : versions) {
			if (!list.empty()) {
				kept.emplace(id, std::move(list));
			}
		}
		versions = std::move(kept);
	}
	if (approximateBytes < 0) {
		approximateBytes = 0;
	}
	if (liveRecords < 0) {
		liveRecords = 0;
	}
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
	startLoad();

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
	TrimVersionList(list, _liveRecords, _approximateBytes);
	enforceLimits();
	compactIfNeeded();
}

void MessageVersions::enforceLimits() {
	TrimToLimits(_versions, _liveRecords, _approximateBytes);
}

void MessageVersions::compactIfNeeded() {
	if (!_loaded) {
		// The file still holds records that are not in memory yet, and a
		// rewrite from memory is exactly what would drop them.
		return;
	} else if (_fileRecords
		<= (_liveRecords * kCompactRatio) + kCompactExtraRecords) {
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
	const auto count = int(records.size());
	if (_owner->session().local().rewriteMessageVersions(
			std::move(records))) {
		_fileRecords = count;
		_liveRecords = count;
	}
}

const std::vector<MessageVersion> *MessageVersions::lookup(FullMsgId id) {
	// Without this the saved file stays unread until something new is
	// captured, so after a restart nothing would ever be found.
	startLoad();

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
	startLoad();
	if (!_loaded) {
		_restoreWhenLoaded.emplace(history->peer->id);
		return;
	}
	restoreNow(history);
}

void MessageVersions::restoreNow(not_null<History*> history) {
	const auto peerId = history->peer->id;
	if (_restoredHistories.contains(peerId)) {
		return;
	}
	_restoredHistories.emplace(peerId);

	const auto owner = &history->owner();
	const auto selfId = history->session().userPeerId();

	// Ids sort by peer first, so one chat's captures are a single range -
	// worth finding instead of walking everything saved for every chat that
	// gets opened. Only regular ids are ever captured, so they are all above
	// the zero one this starts from.
	const auto from = _versions.lower_bound(FullMsgId(peerId, MsgId(0)));
	for (auto i = from; i != _versions.end() && i->first.peer == peerId; ++i) {
		const auto id = i->first;
		const auto &list = i->second;
		if (list.empty()) {
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
	++_loadGeneration; // A read still in flight must not bring it all back.
	_versions.clear();
	_locallyDeleted.clear();
	_restoredHistories.clear();
	_restoreWhenLoaded.clear();
	_approximateBytes = 0;
	_fileRecords = 0;
	_liveRecords = 0;
	_loading = false;
	_loaded = true;
	_owner->session().local().clearMessageVersions();
}

void MessageVersions::startLoad() {
	if (_loaded || _loading) {
		return;
	}
	auto reader = _owner->session().local().messageVersionsReader();
	if (!reader) {
		// No local key yet, so nothing can be read. Left unloaded, to be
		// tried again by whatever asks next.
		return;
	}
	_loading = true;
	const auto generation = ++_loadGeneration;
	auto done = crl::guard(this, [=](
			Map &&loaded,
			int fileRecords,
			int liveRecords,
			int approximateBytes) {
		if (generation != _loadGeneration) {
			return;
		}
		applyLoaded(
			std::move(loaded),
			fileRecords,
			liveRecords,
			approximateBytes);
	});
	crl::async([
		reader = std::move(reader),
		done = std::move(done)
	]() mutable {
		const auto records = reader();
		auto loaded = Map();
		auto liveRecords = 0;
		auto approximateBytes = 0;
		for (const auto &record : records) {
			auto id = FullMsgId();
			auto entry = MessageVersion();
			if (!ParseRecord(record, id, entry)) {
				LOG(("App Error: Bad message version record, stopping."));
				break;
			}
			approximateBytes += ApproximateSize(entry);
			++liveRecords;
			auto &list = loaded[id];
			list.push_back(std::move(entry));
			TrimVersionList(list, liveRecords, approximateBytes);
		}
		TrimToLimits(loaded, liveRecords, approximateBytes);
		crl::on_main([
			done = std::move(done),
			loaded = std::move(loaded),
			fileRecords = int(records.size()),
			liveRecords,
			approximateBytes
		]() mutable {
			done(
				std::move(loaded),
				fileRecords,
				liveRecords,
				approximateBytes);
		});
	});
}

void MessageVersions::applyLoaded(
		Map &&loaded,
		int fileRecords,
		int liveRecords,
		int approximateBytes) {
	_loading = false;
	_loaded = true;

	// Anything captured while the file was being read is newer than all of
	// it, so for each message it goes after what was read.
	for (auto &[id, list] : _versions) {
		if (list.empty()) {
			continue;
		}
		auto &target = loaded[id];
		auto from = list.begin();
		if (!target.empty()
			&& target.back().text == from->text
			&& target.back().tags == from->tags
			&& target.back().deleted == from->deleted) {
			// The read may have caught a capture that had only just been
			// appended, in which case it came back in the records too.
			_approximateBytes -= ApproximateSize(*from);
			--_fileRecords;
			--_liveRecords;
			++from;
		}
		target.insert(
			target.end(),
			std::make_move_iterator(from),
			std::make_move_iterator(list.end()));
	}
	_versions = std::move(loaded);
	_fileRecords += fileRecords;
	_liveRecords += liveRecords;
	_approximateBytes += approximateBytes;

	DEBUG_LOG(("MessageVersions: loaded %1 records, %2 messages."
		).arg(fileRecords
		).arg(_versions.size()));

	enforceLimits();
	compactIfNeeded();

	// The chats opened while the read was in flight were told there was
	// nothing deleted to put back, so they are asked again now.
	for (const auto peerId : base::take(_restoreWhenLoaded)) {
		if (const auto history = _owner->historyLoaded(peerId)) {
			history->checkLocalMessages();
		}
	}
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

} // namespace Data
