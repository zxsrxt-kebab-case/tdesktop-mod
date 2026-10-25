/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mcp/mcp_format.h"

#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

namespace Mcp {
namespace {

constexpr auto kChannelPrefix = qint64(1000000000000LL);

[[nodiscard]] QString MediaName(not_null<Data::Media*> media) {
	if (media->photo()) {
		return u"photo"_q;
	} else if (const auto document = media->document()) {
		return document->sticker()
			? u"sticker"_q
			: document->isVoiceMessage()
			? u"voice"_q
			: document->isVideoMessage()
			? u"video_message"_q
			: document->isAnimation()
			? u"animation"_q
			: document->isVideoFile()
			? u"video"_q
			: document->isAudioFile()
			? u"audio"_q
			: u"file"_q;
	} else if (media->webpage()) {
		return u"webpage"_q;
	} else if (media->poll()) {
		return u"poll"_q;
	}
	return u"other"_q;
}

[[nodiscard]] QString MediaName(const MTPMessageMedia &media) {
	switch (media.type()) {
	case mtpc_messageMediaEmpty: return QString();
	case mtpc_messageMediaPhoto: return u"photo"_q;
	case mtpc_messageMediaDocument: return u"document"_q;
	case mtpc_messageMediaWebPage: return u"webpage"_q;
	case mtpc_messageMediaGeo:
	case mtpc_messageMediaGeoLive:
	case mtpc_messageMediaVenue: return u"location"_q;
	case mtpc_messageMediaContact: return u"contact"_q;
	case mtpc_messageMediaPoll: return u"poll"_q;
	case mtpc_messageMediaDice: return u"dice"_q;
	case mtpc_messageMediaGame: return u"game"_q;
	case mtpc_messageMediaInvoice: return u"invoice"_q;
	case mtpc_messageMediaStory: return u"story"_q;
	case mtpc_messageMediaToDo: return u"todo"_q;
	case mtpc_messageMediaPaidMedia: return u"paid_media"_q;
	}
	return u"other"_q;
}

[[nodiscard]] QString ServiceName(const MTPMessageAction &action) {
	switch (action.type()) {
	case mtpc_messageActionTopicCreate: return u"topic_created"_q;
	case mtpc_messageActionTopicEdit: return u"topic_edited"_q;
	case mtpc_messageActionChatAddUser: return u"members_added"_q;
	case mtpc_messageActionChatDeleteUser: return u"member_removed"_q;
	case mtpc_messageActionChatJoinedByLink: return u"joined_by_link"_q;
	case mtpc_messageActionChatJoinedByRequest:
		return u"joined_by_request"_q;
	case mtpc_messageActionPinMessage: return u"pinned"_q;
	case mtpc_messageActionChatEditTitle: return u"title_changed"_q;
	case mtpc_messageActionChatEditPhoto: return u"photo_changed"_q;
	case mtpc_messageActionChatCreate:
	case mtpc_messageActionChannelCreate: return u"created"_q;
	}
	return u"other"_q;
}

// A message in a forum topic carries the topic as its reply header: a plain
// message points at the topic root, a reply points at the message replied
// to and names the root separately. One without any header is in General.
void ApplyReplyHeader(
		QJsonObject &result,
		const tl::conditional<MTPMessageReplyHeader> &header,
		bool forum) {
	auto topic = MsgId();
	auto replyTo = MsgId();
	if (header) {
		header->match([&](const MTPDmessageReplyHeader &data) {
			const auto top = MsgId(data.vreply_to_top_id().value_or_empty());
			const auto to = MsgId(data.vreply_to_msg_id().value_or_empty());
			if (top) {
				topic = top;
				replyTo = to;
			} else if (data.is_forum_topic()) {
				topic = to;
			} else {
				replyTo = to;
			}
		}, [](const MTPDmessageReplyStoryHeader &) {
		});
	}
	if (forum && !topic) {
		topic = Data::ForumTopic::kGeneralId;
	}
	if (topic) {
		result[u"topic"_q] = qint64(topic.bare);
	}
	if (replyTo) {
		result[u"reply_to"_q] = qint64(replyTo.bare);
	}
}

} // namespace

qint64 BotApiId(PeerId id) {
	return peerToBotApi(id);
}

std::optional<PeerId> PeerFromBotApi(qint64 id) {
	if (id > 0) {
		return peerFromUser(UserId(BareId(id)));
	} else if (id <= -kChannelPrefix) {
		const auto bare = -(id + kChannelPrefix);
		return bare ? std::make_optional(peerFromChannel(ChannelId(BareId(bare)))) : std::nullopt;
	} else if (id < 0) {
		return peerFromChat(ChatId(BareId(-id)));
	}
	return std::nullopt;
}

QString PeerTypeName(not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return u"self"_q;
	} else if (const auto user = peer->asUser()) {
		return user->isBot() ? u"bot"_q : u"user"_q;
	} else if (peer->isChat()) {
		return u"group"_q;
	} else if (peer->isForum()) {
		return u"forum"_q;
	} else if (peer->isMegagroup()) {
		return u"supergroup"_q;
	}
	return u"channel"_q;
}

QJsonObject PeerJson(not_null<PeerData*> peer) {
	auto result = QJsonObject{
		{ u"id"_q, BotApiId(peer->id) },
		{ u"title"_q, peer->name() },
		{ u"type"_q, PeerTypeName(peer) },
	};
	if (const auto username = peer->username(); !username.isEmpty()) {
		result[u"username"_q] = username;
	}
	return result;
}

QJsonObject PeerRef(not_null<Main::Session*> session, PeerId id) {
	auto result = QJsonObject{ { u"id"_q, BotApiId(id) } };
	if (const auto peer = session->data().peerLoaded(id)) {
		result[u"name"_q] = peer->name();
	}
	return result;
}

QString ItemText(not_null<HistoryItem*> item) {
	return item->isService()
		? item->notificationText().text
		: item->originalText().text;
}

QJsonObject ItemJson(not_null<HistoryItem*> item) {
	const auto from = item->from();
	auto result = QJsonObject{
		{ u"id"_q, qint64(item->id.bare) },
		{ u"date"_q, qint64(item->date()) },
		{ u"out"_q, item->out() },
		{ u"from"_q, QJsonObject{
			{ u"id"_q, BotApiId(from->id) },
			{ u"name"_q, from->name() },
		} },
		{ u"text"_q, ItemText(item) },
	};
	if (item->isService()) {
		result[u"service"_q] = true;
	}
	if (const auto replyTo = item->replyToId()) {
		result[u"reply_to"_q] = qint64(replyTo.bare);
	}
	if (const auto topic = item->topicRootId()) {
		result[u"topic"_q] = qint64(topic.bare);
	}
	if (const auto media = item->media()) {
		result[u"media"_q] = MediaName(media);
	}
	return result;
}

QJsonObject MessageJson(
		not_null<Main::Session*> session,
		const MTPMessage &message,
		bool forum) {
	return message.match([&](const MTPDmessage &data) {
		const auto peerId = peerFromMTP(data.vpeer_id());
		const auto fromId = data.vfrom_id()
			? peerFromMTP(*data.vfrom_id())
			: data.is_out()
			? session->userPeerId()
			: peerId;
		auto result = QJsonObject{
			{ u"id"_q, qint64(data.vid().v) },
			{ u"date"_q, qint64(data.vdate().v) },
			{ u"out"_q, data.is_out() },
			{ u"from"_q, PeerRef(session, fromId) },
			{ u"text"_q, qs(data.vmessage()) },
		};
		if (const auto media = data.vmedia()) {
			const auto name = MediaName(*media);
			if (!name.isEmpty()) {
				result[u"media"_q] = name;
			}
		}
		if (data.vedit_date()) {
			result[u"edited"_q] = true;
		}
		if (data.vfwd_from()) {
			result[u"forwarded"_q] = true;
		}
		ApplyReplyHeader(result, data.vreply_to(), forum);
		return result;
	}, [&](const MTPDmessageService &data) {
		const auto peerId = peerFromMTP(data.vpeer_id());
		const auto fromId = data.vfrom_id()
			? peerFromMTP(*data.vfrom_id())
			: peerId;
		auto result = QJsonObject{
			{ u"id"_q, qint64(data.vid().v) },
			{ u"date"_q, qint64(data.vdate().v) },
			{ u"out"_q, data.is_out() },
			{ u"from"_q, PeerRef(session, fromId) },
			{ u"service"_q, ServiceName(data.vaction()) },
		};
		data.vaction().match([&](const MTPDmessageActionTopicCreate &data) {
			result[u"title"_q] = qs(data.vtitle());
		}, [&](const MTPDmessageActionChatEditTitle &data) {
			result[u"title"_q] = qs(data.vtitle());
		}, [](const auto &) {
		});
		ApplyReplyHeader(result, data.vreply_to(), forum);
		return result;
	}, [](const MTPDmessageEmpty &) {
		return QJsonObject();
	});
}

} // namespace Mcp
