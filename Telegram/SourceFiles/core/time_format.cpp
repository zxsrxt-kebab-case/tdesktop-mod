/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/time_format.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtCore/QLocale>

namespace Core {

QString FormatMessageTime(QTime time) {
	const auto locale = QLocale();
	const auto shortFormat = locale.timeFormat(QLocale::ShortFormat);
	if (!Core::App().settings().messageTimeSeconds()) {
		return locale.toString(time, shortFormat);
	}
	// QLocale has no "short format, but with seconds": LongFormat would drag
	// in the timezone as well. So pick the pattern by the locale's clock.
	const auto ampm = shortFormat.contains(u"AP"_q, Qt::CaseInsensitive);
	return locale.toString(
		time,
		ampm ? u"h:mm:ss AP"_q : u"HH:mm:ss"_q);
}

} // namespace Core
