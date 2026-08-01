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
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
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

	const auto unlock = builder.addCheckbox({
		.id = u"modifications/unlock_restricted"_q,
		.title = tr::lng_settings_unlock_restricted(),
		.checked = Core::App().settings().unlockRestrictedContent(),
		.keywords = { u"restricted"_q, u"protected"_q, u"copy"_q, u"save"_q,
			u"noforwards"_q, u"forward"_q },
	});
	if (unlock) {
		unlock->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			Core::App().settings().setUnlockRestrictedContent(checked);
			Core::App().saveSettingsDelayed();
		}, unlock->lifetime());
	}
	builder.addDividerText(tr::lng_settings_unlock_restricted_about());
}

// A titled slider over a range of pixel sizes, laid out the way the local
// storage limits are: name on the left, current value on the right.
void AddSizeSlider(
		SectionBuilder &builder,
		const QString &id,
		rpl::producer<QString> title,
		QString label,
		QStringList keywords,
		rpl::producer<bool> shown,
		int min,
		int max,
		int step,
		int startValue,
		Fn<void(int)> save) {
	builder.addControl({
		.factory = [=](not_null<Ui::VerticalLayout*> parent)
		-> object_ptr<Ui::RpWidget> {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			const auto raw = result.data();
			const auto row = raw->add(
				object_ptr<Ui::FixedHeightWidget>(
					raw,
					st::modificationsSliderLabel.font->height),
				st::modificationsSliderLabelMargin);
			const auto name = Ui::CreateChild<Ui::LabelSimple>(
				row,
				st::modificationsSliderLabel,
				label);
			const auto current = Ui::CreateChild<Ui::LabelSimple>(
				row,
				st::modificationsSliderValue);
			rpl::combine(
				row->widthValue(),
				current->widthValue()
			) | rpl::on_next([=](int width, int) {
				name->moveToLeft(0, 0, width);
				current->moveToRight(0, 0, width);
			}, row->lifetime());

			const auto show = [=](int value) {
				current->setText(QString::number(value) + u"px"_q);
			};
			show(startValue);

			const auto slider = raw->add(
				object_ptr<Ui::MediaSlider>(raw, st::modificationsSlider),
				st::modificationsSliderMargin);
			slider->resize(st::modificationsSlider.seekSize);
			slider->setPseudoDiscrete(
				(max - min) / step + 1,
				[=](int index) { return min + index * step; },
				startValue,
				[=](int value) {
					show(value);
					save(value);
				});
			return result;
		},
		.id = id,
		.title = std::move(title),
		.shown = std::move(shown),
		.keywords = std::move(keywords),
	});
}

void BuildNotificationsSection(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSubsectionTitle(
		tr::lng_settings_modifications_notifications());

	auto &settings = Core::App().settings();
	const auto compact = builder.addCheckbox({
		.id = u"modifications/compact_notifications"_q,
		.title = tr::lng_settings_compact_notifications(),
		.checked = settings.compactNotifications(),
		.keywords = { u"notification"_q, u"compact"_q, u"popup"_q,
			u"toast"_q, u"rounded"_q },
	});
	if (compact) {
		compact->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			Core::App().settings().setCompactNotifications(checked);
			Core::App().saveSettingsDelayed();
		}, compact->lifetime());
	}

	using Settings = Core::Settings;
	const auto sizeKeywords = QStringList{
		u"notification"_q, u"size"_q, u"width"_q, u"height"_q, u"radius"_q,
	};
	AddSizeSlider(
		builder,
		u"modifications/compact_notification_width"_q,
		tr::lng_settings_compact_notification_width(),
		tr::lng_settings_compact_notification_width(tr::now),
		sizeKeywords,
		settings.compactNotificationsValue(),
		Settings::kCompactNotificationWidthMin,
		Settings::kCompactNotificationWidthMax,
		10,
		settings.compactNotificationWidth(),
		[](int value) {
			Core::App().settings().setCompactNotificationWidth(value);
			Core::App().saveSettingsDelayed();
		});
	AddSizeSlider(
		builder,
		u"modifications/compact_notification_height"_q,
		tr::lng_settings_compact_notification_height(),
		tr::lng_settings_compact_notification_height(tr::now),
		sizeKeywords,
		settings.compactNotificationsValue(),
		Settings::kCompactNotificationHeightMin,
		Settings::kCompactNotificationHeightMax,
		4,
		settings.compactNotificationHeight(),
		[](int value) {
			Core::App().settings().setCompactNotificationHeight(value);
			Core::App().saveSettingsDelayed();
		});
	AddSizeSlider(
		builder,
		u"modifications/compact_notification_radius"_q,
		tr::lng_settings_compact_notification_radius(),
		tr::lng_settings_compact_notification_radius(tr::now),
		sizeKeywords,
		settings.compactNotificationsValue(),
		0,
		Settings::kCompactNotificationRadiusMax,
		2,
		settings.compactNotificationRadius(),
		[](int value) {
			Core::App().settings().setCompactNotificationRadius(value);
			Core::App().saveSettingsDelayed();
		});

	builder.addDividerText(tr::lng_settings_compact_notifications_about());
}

void BuildHistorySection(SectionBuilder &builder) {
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
	BuildNotificationsSection(builder);
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
