/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_modifications.h"

#include "settings/settings_common_session.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/streamer_mode.h"
#include "lang/lang_keys.h"
#include "settings/settings_builder.h"
#include "settings/sections/settings_main.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

void BuildPrivacySection(SectionBuilder &builder) {
	builder.addSubsectionTitle(tr::lng_settings_modifications_privacy());

	const auto ghost = builder.addCheckbox({
		.id = u"modifications/ghost_mode"_q,
		.title = tr::lng_settings_ghost_mode(),
		.checked = Core::App().settings().ghostMode(),
		.keywords = { u"ghost"_q, u"read"_q, u"receipts"_q, u"typing"_q,
			u"online"_q, u"invisible"_q },
	});
	if (ghost) {
		ghost->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			Core::App().settings().setGhostMode(checked);
			Core::App().saveSettingsDelayed();
		}, ghost->lifetime());
	}

	if (Core::StreamerMode::Supported()) {
		const auto streamer = builder.addCheckbox({
			.id = u"modifications/streamer_mode"_q,
			.title = tr::lng_settings_streamer_mode(),
			.checked = Core::App().settings().streamerMode(),
			.keywords = { u"streamer"_q, u"stream"_q, u"capture"_q,
				u"recording"_q, u"obs"_q },
		});
		if (streamer) {
			streamer->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				Core::App().settings().setStreamerMode(checked);
				Core::App().saveSettingsDelayed();
				Core::App().refreshStreamerMode();
			}, streamer->lifetime());
		}
	}
}

void BuildHistorySection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSubsectionTitle(tr::lng_settings_modifications_history());

	const auto save = builder.addCheckbox({
		.id = u"modifications/save_message_versions"_q,
		.title = tr::lng_settings_save_message_versions(),
		.checked = Core::App().settings().saveMessageVersions(),
		.keywords = { u"history"_q, u"edited"_q, u"deleted"_q, u"versions"_q },
	});
	if (save) {
		save->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			Core::App().settings().setSaveMessageVersions(checked);
			Core::App().saveSettingsDelayed();
		}, save->lifetime());
	}
	builder.addDividerText(tr::lng_settings_save_message_versions_about());
}

void BuildAppearanceSection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSubsectionTitle(tr::lng_settings_modifications_appearance());

	const auto seconds = builder.addCheckbox({
		.id = u"modifications/message_seconds"_q,
		.title = tr::lng_settings_message_seconds(),
		.checked = Core::App().settings().messageTimeSeconds(),
		.keywords = { u"seconds"_q, u"time"_q, u"clock"_q },
	});
	if (seconds) {
		seconds->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			Core::App().settings().setMessageTimeSeconds(checked);
			Core::App().saveSettingsDelayed();
		}, seconds->lifetime());
	}
}

class Modifications : public Section<Modifications> {
public:
	Modifications(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = Modifications::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_modifications,
	.icon = &st::menuIconStats,
}, [](SectionBuilder &builder) {
	BuildPrivacySection(builder);
	BuildHistorySection(builder);
	BuildAppearanceSection(builder);
});

const SectionBuildMethod kModificationsSection = kMeta.build;

Modifications::Modifications(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Modifications::title() {
	return tr::lng_settings_modifications();
}

void Modifications::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kModificationsSection);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type ModificationsId() {
	return Modifications::Id();
}

} // namespace Settings
