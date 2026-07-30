/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class QWidget;

namespace Core::StreamerMode {

// Hides the application's windows from screen capture and recording, so that
// they do not show up in a stream. Only Windows can actually do this; on the
// other platforms the calls are no-ops.
[[nodiscard]] bool Supported();

// Applies the current setting to one window, for windows created later.
void Apply(QWidget *widget);

} // namespace Core::StreamerMode
