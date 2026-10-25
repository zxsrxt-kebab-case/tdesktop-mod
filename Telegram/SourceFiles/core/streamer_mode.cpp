/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/streamer_mode.h"

#include "core/application.h"
#include "core/core_screenshot_protection.h"
#include "core/core_settings.h"
#include "platform/platform_specific.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace Core::StreamerMode {
namespace {

// Streamer mode covers the application, not a list of windows. The media
// viewer, notifications, call panels, menus and tooltips all show what is in
// a chat, and all of them come and go while the mode is on, so the windows
// are watched for instead of enumerated once.
//
// Each one gets its own reason in the screenshot protection registry, which
// is what decides the final state per window - so this and the protection a
// window asks for itself never overwrite each other.
class Watcher final : public QObject {
public:
	explicit Watcher(not_null<QObject*> parent);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void setEnabled(bool enabled);
	void track(not_null<QWidget*> window);

	// The address of a window that is gone can be handed to the next one, so
	// what is tracked is remembered by address and checked for being alive.
	struct Tracked {
		QPointer<QWidget> alive;
		rpl::lifetime lifetime;
	};
	base::flat_map<QWidget*, Tracked> _tracked;
	bool _enabled = false;
	rpl::lifetime _lifetime;

};

Watcher::Watcher(not_null<QObject*> parent)
: QObject(parent) {
	App().settings().streamerModeValue(
	) | rpl::on_next([=](bool enabled) {
		setEnabled(enabled);
	}, _lifetime);
}

void Watcher::setEnabled(bool enabled) {
	if (_enabled == enabled) {
		return;
	}
	_enabled = enabled;
	if (!enabled) {
		// Dropping the reasons is what makes the windows capturable again.
		_tracked.clear();
		qApp->removeEventFilter(this);
		return;
	}

	// A filter on the application object sees the events of every object,
	// which is the only way to hear about a window the application does not
	// own itself, like a menu or a tooltip from the UI library.
	qApp->installEventFilter(this);
	for (const auto widget : QApplication::topLevelWidgets()) {
		track(widget);
	}
}

bool Watcher::eventFilter(QObject *watched, QEvent *event) {
	if (event->type() == QEvent::Show) {
		if (const auto widget = qobject_cast<QWidget*>(watched)) {
			if (widget->isWindow()) {
				track(widget);
			}
		}
	}
	return QObject::eventFilter(watched, event);
}

void Watcher::track(not_null<QWidget*> window) {
	const auto raw = window.get();
	const auto i = _tracked.find(raw);
	if (i != _tracked.end()) {
		if (i->second.alive) {
			return;
		}
		_tracked.erase(i);
	}
	auto &tracked = _tracked[raw];
	tracked.alive = raw;
	App().screenshotProtection().addContentReason(
		window,
		rpl::single(true),
		tracked.lifetime);
}

} // namespace

bool Supported() {
	return Platform::ScreenshotProtectionSupported();
}

void Start() {
	if (!Supported()) {
		return;
	}

	// Parented to the application object, which outlives every window it is
	// going to hear about.
	new Watcher(qApp);
}

} // namespace Core::StreamerMode
