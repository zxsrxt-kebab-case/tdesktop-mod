/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace Core {

// Resolves the name of the directory that keeps all the local data.
//
// Instead of the well-known 'tdata' name a random one is generated on the
// first launch, so that a tool which blindly copies '<workdir>/tdata' finds
// nothing. An already existing 'tdata' is renamed on the first launch.
//
// Must be called once the working dir is final and before anything touches
// the data directory. Repeated calls do nothing.
void InitDataDirectory();

// Name of the data directory inside cWorkingDir(), without any slashes.
[[nodiscard]] const QString &DataDirName();

// cWorkingDir() + DataDirName() + '/'.
[[nodiscard]] QString DataPath();

// cWorkingDir() + DataDirName() + '/' + relative.
[[nodiscard]] QString DataPath(const QString &relative);

} // namespace Core
