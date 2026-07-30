/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace HistoryView {

// Lists the locally kept earlier versions of a message - what it said before
// it was edited, and what it said before it was deleted.
void MessageVersionsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session,
	FullMsgId id);

} // namespace HistoryView
