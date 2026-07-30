/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"

namespace Settings {

// Settings that do not come from upstream Telegram Desktop, collected in one
// place instead of being scattered across the stock sections.
[[nodiscard]] Type ModificationsId();

} // namespace Settings
