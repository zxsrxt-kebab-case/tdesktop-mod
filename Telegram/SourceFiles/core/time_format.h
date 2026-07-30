/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QTime>

namespace Core {

// Formats a timestamp the way message bubbles and online statuses show it,
// honouring the "show seconds" setting.
[[nodiscard]] QString FormatMessageTime(QTime time);

} // namespace Core
