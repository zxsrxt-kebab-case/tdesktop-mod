/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_mcp.h"

#include "settings/settings_common_session.h"

#include "core/application.h"
#include "lang/lang_keys.h"
#include "mcp/mcp_manager.h"
#include "settings/settings_builder.h"
#include "settings/sections/settings_modifications.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Settings {
namespace {

using namespace Builder;

constexpr auto kServerName = "kebab";
constexpr auto kMaskLength = 32;

// A value shown the way code is: in a monospace font on a rounded plate,
// with what a click does written on the right. A masked one shows dots
// until it is clicked once.
class CodeField final : public Ui::AbstractButton {
public:
	CodeField(
		QWidget *parent,
		rpl::producer<QString> text,
		bool masked);

	[[nodiscard]] bool hidden() const;
	void reveal();

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	QString _text;
	bool _masked = false;
	bool _revealed = false;

};

class StatusLine final : public Ui::RpWidget {
public:
	StatusLine(QWidget *parent, rpl::producer<Mcp::Status> status);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void apply(const Mcp::Status &status);

	QString _text;
	Mcp::State _state = Mcp::State::Off;

};

CodeField::CodeField(
	QWidget *parent,
	rpl::producer<QString> text,
	bool masked)
: AbstractButton(parent)
, _masked(masked) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		update();
	}, lifetime());
	setCursor(style::cur_pointer);
}

bool CodeField::hidden() const {
	return _masked && !_revealed;
}

void CodeField::reveal() {
	_revealed = true;
	update();
}

int CodeField::resizeGetHeight(int newWidth) {
	return st::normalFont->height + 2 * st::settingsButtonNoIcon.padding.top();
}

void CodeField::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(isOver() ? st::windowBgRipple : st::windowBgOver);
		p.drawRoundedRect(
			rect(),
			st::roundRadiusLarge,
			st::roundRadiusLarge);
	}
	const auto skip = st::settingsButtonNoIcon.padding.top();
	const auto &font = st::normalFont;
	const auto mono = font->monospace();
	const auto hint = hidden()
		? tr::lng_settings_mcp_click_show(tr::now)
		: tr::lng_settings_mcp_click_copy(tr::now);
	const auto hintWidth = font->width(hint);
	const auto available = std::max(width() - 3 * skip - hintWidth, 0);
	const auto shown = hidden()
		? QString(std::min(int(_text.size()), kMaskLength), QChar(0x2022))
		: _text;

	p.setFont(mono);
	p.setPen(st::windowFg);
	p.drawText(
		skip,
		(height() - mono->height) / 2 + mono->ascent,
		mono->elided(shown, available));

	p.setFont(font);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		width() - skip - hintWidth,
		(height() - font->height) / 2 + font->ascent,
		hint);
}

StatusLine::StatusLine(QWidget *parent, rpl::producer<Mcp::Status> status)
: RpWidget(parent) {
	std::move(status) | rpl::on_next([=](const Mcp::Status &status) {
		apply(status);
	}, lifetime());
}

void StatusLine::apply(const Mcp::Status &status) {
	_state = status.state;
	const auto address = u"127.0.0.1:%1"_q.arg(status.port);
	switch (status.state) {
	case Mcp::State::Off:
		_text = tr::lng_settings_mcp_status_off(tr::now);
		break;
	case Mcp::State::Locked:
		_text = tr::lng_settings_mcp_status_locked(tr::now);
		break;
	case Mcp::State::Failed:
		_text = tr::lng_settings_mcp_status_failed(
			tr::now,
			lt_error,
			status.error);
		break;
	case Mcp::State::Running:
		_text = status.streams
			? tr::lng_settings_mcp_status_streams(
				tr::now,
				lt_address,
				address,
				lt_streams,
				QString::number(status.streams))
			: tr::lng_settings_mcp_status_running(
				tr::now,
				lt_address,
				address);
		break;
	}
	update();
}

int StatusLine::resizeGetHeight(int newWidth) {
	return st::normalFont->height + st::settingsButtonNoIcon.padding.top();
}

void StatusLine::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto &font = st::normalFont;
	const auto size = font->height / 2;
	const auto top = (height() - font->height) / 2;
	const auto color = [&] {
		switch (_state) {
		case Mcp::State::Running: return st::boxTextFgGood->c;
		case Mcp::State::Failed: return st::boxTextFgError->c;
		case Mcp::State::Locked: return st::windowActiveTextFg->c;
		case Mcp::State::Off: break;
		}
		return st::windowSubTextFg->c;
	}();
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(0, top + (font->height - size) / 2, size, size);
	}
	const auto left = size * 2;
	p.setFont(font);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		left,
		top + font->ascent,
		font->elided(_text, std::max(width() - left, 0)));
}

[[nodiscard]] QString ExecutablePath() {
	return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

[[nodiscard]] QString HttpCommand(not_null<Mcp::Manager*> manager) {
	const auto &settings = manager->settings();
	return u"claude mcp add --transport http %1 %2 "
		"--header \"Authorization: Bearer %3\""_q
		.arg(QString::fromLatin1(kServerName))
		.arg(manager->address())
		.arg(QString::fromLatin1(settings.token()));
}

[[nodiscard]] QString BridgeCommand(not_null<Mcp::Manager*> manager) {
	const auto &settings = manager->settings();
	return u"claude mcp add --transport stdio --env KEBAB_MCP_TOKEN=%1 %2 "
		"-- \"%3\" -mcpbridge %4"_q
		.arg(QString::fromLatin1(settings.token()))
		.arg(QString::fromLatin1(kServerName))
		.arg(ExecutablePath())
		.arg(settings.port());
}

[[nodiscard]] QString ChannelCommand() {
	return u"claude --dangerously-load-development-channels server:%1"_q
		.arg(QString::fromLatin1(kServerName));
}

void Copy(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	QGuiApplication::clipboard()->setText(text);
	controller->showToast(tr::lng_settings_mcp_copied(tr::now));
}

void PortBox(not_null<Ui::GenericBox*> box, not_null<Mcp::Manager*> manager) {
	box->setTitle(tr::lng_settings_mcp_port());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		tr::lng_settings_mcp_port(),
		QString::number(manager->settings().port())));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_settings_mcp_port_about(),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	const auto save = [=] {
		auto ok = false;
		const auto port = field->getLastText().trimmed().toInt(&ok);
		if (!ok || port < Mcp::kMinPort || port > Mcp::kMaxPort) {
			field->showError();
			return;
		}
		manager->settings().setPort(port);
		box->closeBox();
	};
	field->submits() | rpl::on_next(save, field->lifetime());
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void BuildServerSection(
		SectionBuilder &builder,
		not_null<Mcp::Manager*> manager) {
	auto &settings = manager->settings();

	builder.addSkip();
	const auto enable = builder.addButton({
		.id = u"mcp/enabled"_q,
		.title = tr::lng_settings_mcp_enable(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings.enabled()),
		.keywords = { u"mcp"_q, u"claude"_q, u"server"_q, u"api"_q },
	});
	if (enable) {
		enable->toggledChanges(
		) | rpl::on_next([=](bool enabled) {
			manager->settings().setEnabled(enabled);
		}, enable->lifetime());
	}
	builder.add([=](const WidgetContext &ctx) {
		const auto padding = st::settingsButtonNoIcon.padding;
		return SectionBuilder::WidgetToAdd{
			.widget = object_ptr<StatusLine>(
				ctx.container,
				manager->statusValue()),
			.margin = QMargins(padding.left(), 0, padding.right(), 0),
		};
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_settings_mcp_about());
}

void BuildConnectionSection(
		SectionBuilder &builder,
		not_null<Mcp::Manager*> manager) {
	auto &settings = manager->settings();
	const auto controller = builder.controller();
	const auto margin = [] {
		const auto padding = st::settingsButtonNoIcon.padding;
		return QMargins(padding.left(), 0, padding.right(), 0);
	};

	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"mcp/address"_q,
		.title = tr::lng_settings_mcp_address(),
		.keywords = { u"ip"_q, u"url"_q, u"localhost"_q },
	});
	builder.add([=](const WidgetContext &ctx) {
		auto field = object_ptr<CodeField>(
			ctx.container,
			manager->settings().portValue() | rpl::map([=](int) {
				return manager->address();
			}),
			false);
		field->setClickedCallback([=] {
			Copy(ctx.controller, manager->address());
		});
		return SectionBuilder::WidgetToAdd{
			.widget = std::move(field),
			.margin = margin(),
		};
	});
	builder.addSkip();
	builder.addButton({
		.id = u"mcp/port"_q,
		.title = tr::lng_settings_mcp_port(),
		.st = &st::settingsButtonNoIcon,
		.label = settings.portValue() | rpl::map([](int port) {
			return QString::number(port);
		}),
		.onClick = [=] {
			if (controller) {
				controller->show(Box(PortBox, manager));
			}
		},
	});

	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"mcp/token"_q,
		.title = tr::lng_settings_mcp_token(),
		.keywords = { u"secret"_q, u"key"_q, u"password"_q, u"bearer"_q },
	});
	builder.add([=](const WidgetContext &ctx) {
		auto field = object_ptr<CodeField>(
			ctx.container,
			manager->settings().tokenValue(
			) | rpl::map([](const QByteArray &token) {
				return QString::fromLatin1(token);
			}),
			true);
		const auto raw = field.data();
		field->setClickedCallback([=] {
			if (raw->hidden()) {
				raw->reveal();
			} else {
				const auto token = manager->settings().token();
				Copy(ctx.controller, QString::fromLatin1(token));
			}
		});
		return SectionBuilder::WidgetToAdd{
			.widget = std::move(field),
			.margin = margin(),
		};
	});
	builder.addSkip();
	builder.addButton({
		.id = u"mcp/token-new"_q,
		.title = tr::lng_settings_mcp_token_new(),
		.st = &st::settingsAttentionButton,
		.onClick = [=] {
			if (!controller) {
				return;
			}
			controller->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_mcp_token_new_sure(),
				.confirmed = [=](Fn<void()> close) {
					manager->settings().regenerateToken();
					close();
				},
				.confirmText = tr::lng_settings_mcp_token_new_confirm(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		},
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_settings_mcp_token_about());
}

void BuildPermissionsSection(
		SectionBuilder &builder,
		not_null<Mcp::Manager*> manager) {
	using Capability = Mcp::Capability;
	auto &settings = manager->settings();

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_settings_mcp_permissions());
	const auto add = [&](
			Capability capability,
			QString id,
			rpl::producer<QString> title,
			const style::icon *icon) {
		const auto button = builder.addButton({
			.id = u"mcp/"_q + id,
			.title = std::move(title),
			.st = &st::settingsButton,
			.icon = { icon },
			.toggled = rpl::single(settings.has(capability)),
		});
		if (button) {
			button->toggledChanges(
			) | rpl::on_next([=](bool enabled) {
				manager->settings().setCapability(capability, enabled);
			}, button->lifetime());
		}
	};
	add(
		Capability::Read,
		u"read"_q,
		tr::lng_settings_mcp_cap_read(),
		&st::menuIconChatBubble);
	add(
		Capability::Drafts,
		u"drafts"_q,
		tr::lng_settings_mcp_cap_drafts(),
		&st::menuIconEdit);
	add(
		Capability::Send,
		u"send"_q,
		tr::lng_settings_mcp_cap_send(),
		&st::menuIconSend);
	add(
		Capability::Manage,
		u"manage"_q,
		tr::lng_settings_mcp_cap_manage(),
		&st::menuIconDelete);
	add(
		Capability::Events,
		u"events"_q,
		tr::lng_settings_mcp_cap_events(),
		&st::menuIconNotifications);
	add(
		Capability::Client,
		u"client"_q,
		tr::lng_settings_mcp_cap_client(),
		&st::menuIconCustomize);
	builder.addSkip();
	builder.addDividerText(tr::lng_settings_mcp_permissions_about());
}

void BuildClaudeSection(
		SectionBuilder &builder,
		not_null<Mcp::Manager*> manager) {
	const auto controller = builder.controller();

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_settings_mcp_claude());
	builder.addButton({
		.id = u"mcp/copy-http"_q,
		.title = tr::lng_settings_mcp_copy_http(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconCopy },
		.onClick = [=] {
			if (controller) {
				Copy(controller, HttpCommand(manager));
			}
		},
		.keywords = { u"claude"_q, u"command"_q },
	});
	builder.addButton({
		.id = u"mcp/copy-bridge"_q,
		.title = tr::lng_settings_mcp_copy_bridge(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconNotifications },
		.onClick = [=] {
			if (controller) {
				Copy(controller, BridgeCommand(manager));
			}
		},
		.keywords = { u"claude"_q, u"channel"_q, u"push"_q },
	});
	builder.addButton({
		.id = u"mcp/copy-channel"_q,
		.title = tr::lng_settings_mcp_copy_channel(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconTopics },
		.onClick = [=] {
			if (controller) {
				Copy(controller, ChannelCommand());
			}
		},
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_settings_mcp_claude_about());
}

class McpSection : public Section<McpSection> {
public:
	McpSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = McpSection::Id(),
	.parentId = ModificationsId(),
	.title = &tr::lng_settings_mcp,
	.icon = &st::menuIconBotCommands,
}, [](SectionBuilder &builder) {
	const auto manager = Core::App().mcp();
	if (!manager) {
		return;
	}
	BuildServerSection(builder, manager);
	BuildConnectionSection(builder, manager);
	BuildPermissionsSection(builder, manager);
	BuildClaudeSection(builder, manager);
});

const SectionBuildMethod kMcpSection = kMeta.build;

McpSection::McpSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> McpSection::title() {
	return tr::lng_settings_mcp();
}

void McpSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kMcpSection);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type McpId() {
	return McpSection::Id();
}

} // namespace Settings
