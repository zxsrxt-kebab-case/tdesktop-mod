/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/streamer_mode.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtWidgets/QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Core::StreamerMode {
namespace {

void ApplyToHandle(QWidget *widget, bool hide) {
#ifdef Q_OS_WIN
	if (!widget) {
		return;
	}
	const auto window = widget->window();
	if (!window) {
		return;
	}
	// WDA_EXCLUDEFROMCAPTURE keeps the window on screen for the user while
	// making it invisible to capture APIs. It needs Windows 10 2004 or newer;
	// on older builds the call simply fails and the window stays capturable.
	const auto handle = reinterpret_cast<HWND>(window->winId());
	SetWindowDisplayAffinity(
		handle,
		hide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
#endif // Q_OS_WIN
}

} // namespace

bool Supported() {
#ifdef Q_OS_WIN
	return true;
#else // Q_OS_WIN
	// X11 and Wayland have no equivalent, and the macOS way needs Cocoa,
	// which would have to live in an Objective-C++ file.
	return false;
#endif // !Q_OS_WIN
}

void Apply(QWidget *widget) {
	ApplyToHandle(widget, Core::App().settings().streamerMode());
}

} // namespace Core::StreamerMode
