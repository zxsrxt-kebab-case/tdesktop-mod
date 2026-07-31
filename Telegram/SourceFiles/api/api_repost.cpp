/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_repost.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "ui/text/text_entity.h"
#include "ui/text/text_utilities.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

namespace Api {
namespace {

// The original text under a bold author line, all of it inside a blockquote,
// so that a repost still reads as a citation and not as our own words.
[[nodiscard]] TextWithEntities ComposeQuote(not_null<HistoryItem*> item) {
	auto result = TextWithEntities();
	result.append(Ui::Text::Bold(item->author()->name()));
	const auto &original = item->originalText();
	if (!original.text.isEmpty()) {
		result.append(u"\n"_q).append(original);
	}
	result.entities.insert(
		result.entities.begin(),
		EntityInText(EntityType::Blockquote, 0, int(result.text.size())));
	return result;
}

} // namespace

bool CanRepost(not_null<HistoryItem*> item) {
	if (item->isService()) {
		return false;
	}
	const auto media = item->media();
	if (media && (media->photo() || media->document())) {
		return true;
	}
	// Polls, contacts, locations and the like have no send-as-new counterpart,
	// so such a message is worth reposting only for the text it carries.
	return !item->originalText().text.isEmpty();
}

void RepostMessageTo(
		not_null<Data::Thread*> thread,
		not_null<HistoryItem*> item) {
	const auto session = &thread->owningHistory()->session();
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;

	auto quote = ComposeQuote(item);

	auto action = SendAction(thread);
	action.clearDraft = false;

	const auto sendText = [&](TextWithEntities &&text) {
		auto message = MessageToSend(action);
		message.textWithTags = TextWithTags{
			text.text,
			TextUtilities::ConvertEntitiesToTextTags(text.entities),
		};
		session->api().sendMessage(std::move(message));
	};

	if (!photo && !document) {
		if (!quote.text.isEmpty()) {
			sendText(std::move(quote));
		}
		return;
	}

	// A caption is capped far lower than a message, so an overlong quote
	// follows the media as a message of its own instead of being cut.
	const auto fits = (int(quote.text.size())
		<= session->serverConfig().captionLengthMax);
	auto caption = fits ? std::move(quote) : TextWithEntities();

	auto message = MessageToSend(action);
	message.textWithTags = TextWithTags{
		caption.text,
		TextUtilities::ConvertEntitiesToTextTags(caption.entities),
	};
	if (photo) {
		SendExistingPhoto(std::move(message), photo);
	} else {
		SendExistingDocument(std::move(message), document);
	}
	if (!fits) {
		sendText(std::move(quote));
	}
}

void RepostMessage(
		not_null<Window::SessionNavigation*> navigation,
		not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto itemId = item->fullId();
	const auto weak = base::make_weak(navigation);
	Window::ShowChooseRecipientBox(navigation, [=](
			not_null<Data::Thread*> thread) {
		if (const auto item = session->data().message(itemId)) {
			RepostMessageTo(thread, item);
			if (const auto strong = weak.get()) {
				strong->showThread(thread);
			}
		}
		return true;
	});
}

} // namespace Api
