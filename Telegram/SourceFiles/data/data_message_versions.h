/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"

class HistoryItem;
class History;

namespace Main {
class Session;
} // namespace Main

namespace Data {

class Session;

// One captured state of a message: what it said before it was edited, or
// what it said before it was deleted.
struct MessageVersion {
	PeerId authorId = 0;
	TimeId originalDate = 0; // Of the message, for placing it in the list.
	TimeId capturedAt = 0;
	QString text;
	QByteArray tags; // Formatting, serialized the way drafts store it.
	bool deleted = false;
};

// Keeps local copies of messages that were edited or deleted, so that the
// earlier text does not disappear along with the server's copy.
//
// Records are appended to one encrypted file, under the same local key as
// the rest of the account data - it is not a separate plaintext database.
// Appending costs the same whatever has piled up; the file is compacted
// when it holds noticeably more records than are still live.
class MessageVersions final {
public:
	explicit MessageVersions(not_null<Session*> owner);
	~MessageVersions();

	// Call with the item still holding its old text.
	void captureBeforeEdit(not_null<HistoryItem*> item);
	void captureBeforeDelete(not_null<HistoryItem*> item);

	[[nodiscard]] const std::vector<MessageVersion> *lookup(FullMsgId id);
	[[nodiscard]] bool has(FullMsgId id);

	// Messages the server deleted but we kept on screen with a badge.
	void markLocallyDeleted(FullMsgId id);
	[[nodiscard]] bool locallyDeleted(FullMsgId id) const;

	// Recreates messages the server deleted as local ones, so that they come
	// back after a restart. tdesktop keeps no local copy of the history, so
	// without this they are gone as soon as it is refetched.
	void restoreInto(not_null<History*> history);

	void clear();

private:
	void load();
	void capture(not_null<HistoryItem*> item, bool deleted);
	void enforceLimits();
	void compactIfNeeded();

	[[nodiscard]] QByteArray serializeRecord(
		FullMsgId id,
		const MessageVersion &version) const;
	[[nodiscard]] bool applyRecord(const QByteArray &record);

	const not_null<Session*> _owner;
	base::flat_map<FullMsgId, std::vector<MessageVersion>> _versions;
	base::flat_set<FullMsgId> _locallyDeleted;
	base::flat_set<PeerId> _restoredHistories;
	int _approximateBytes = 0;
	int _fileRecords = 0; // Including ones no longer live, for compaction.
	int _liveRecords = 0;
	bool _loaded = false;

};

} // namespace Data
