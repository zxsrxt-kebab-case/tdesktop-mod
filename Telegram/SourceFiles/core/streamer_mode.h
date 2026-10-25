/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core::StreamerMode {

// Hides the application's windows from screen capture and recording, so that
// they do not show up in a stream. Windows and macOS can do this; X11 and
// Wayland have no equivalent, so the setting is not offered there.
[[nodiscard]] bool Supported();

// Starts following the setting. Called once, from Application::run().
void Start();

} // namespace Core::StreamerMode
