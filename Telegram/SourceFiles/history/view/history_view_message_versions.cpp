/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_message_versions.h"

#include "base/unixtime.h"
#include "data/data_message_versions.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace HistoryView {
namespace {

[[nodiscard]] QString VersionHeader(
		not_null<Main::Session*> session,
		const Data::MessageVersion &version) {
	const auto when = QLocale().toString(
		base::unixtime::parse(version.capturedAt),
		QLocale::ShortFormat);
	const auto author = session->data().peerLoaded(version.authorId);
	const auto name = author ? author->name() : QString();
	const auto what = version.deleted
		? tr::lng_message_versions_deleted(tr::now)
		: tr::lng_message_versions_edited(tr::now);
	return name.isEmpty() ? (what + u" - "_q + when) : (name
		+ u" - "_q
		+ what
		+ u" - "_q
		+ when);
}

} // namespace

void MessageVersionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		FullMsgId id) {
	box->setTitle(tr::lng_message_versions_title());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	const auto versions = session->data().messageVersions().lookup(id);
	if (!versions) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_message_versions_empty(tr::now),
			st::boxLabel));
		return;
	}
	const auto container = box->verticalLayout();
	auto first = true;
	for (const auto &version : *versions) {
		if (!first) {
			Ui::AddSkip(container);
			Ui::AddDivider(container);
		}
		first = false;
		Ui::AddSkip(container);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				VersionHeader(session, version),
				st::boxDividerLabel),
			st::boxRowPadding);
		Ui::AddSkip(container);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(TextWithEntities{ version.text }),
				st::boxLabel),
			st::boxRowPadding);
		Ui::AddSkip(container);
	}
}

} // namespace HistoryView
