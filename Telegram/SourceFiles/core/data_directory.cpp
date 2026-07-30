/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/data_directory.h"

#include "base/random.h"
#include "settings.h" // cWorkingDir.

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>

namespace Core {
namespace {

// The name used by the unmodified application.
constexpr auto kLegacyName = "tdata";

// Keeps the name of the current data directory, lives in the working dir.
constexpr auto kPointerName = ".data";

constexpr auto kNameLength = 16;

// Directories in the working dir that are never the data directory.
const char *kNeverData[] = { "tupdates", "DebugLogs", "Downloads" };

// Files that only the data directory can contain.
const char *kDataMarkers[] = {
	"key_data0", "key_data1", "key_datas",
	"settings0", "settings1", "settingss",
	"usertag", "prefix",
};

// The working dir GlobalName was resolved against. It can still change
// after an early resolve, for example by a '-workdir' argument.
QString GlobalWorking;
QString GlobalName;

[[nodiscard]] bool ValidName(const QString &name) {
	static const auto expression = QRegularExpression(
		u"^[a-zA-Z0-9_\\-]{1,64}$"_q);
	return expression.match(name).hasMatch();
}

[[nodiscard]] QString GenerateName() {
	const auto chars = u"abcdefghijklmnopqrstuvwxyz0123456789"_q;
	auto random = QByteArray(kNameLength, Qt::Uninitialized);
	base::RandomFill(random.data(), random.size());

	auto result = QString();
	result.reserve(kNameLength);
	for (const auto value : random) {
		result.append(chars[uchar(value) % chars.size()]);
	}
	return result;
}

[[nodiscard]] bool LooksLikeData(const QString &path) {
	for (const auto marker : kDataMarkers) {
		if (QFile::exists(path + '/' + QString::fromUtf8(marker))) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool NeverData(const QString &name) {
	for (const auto entry : kNeverData) {
		if (name == QString::fromUtf8(entry)) {
			return true;
		}
	}
	return false;
}

// Used when the pointer file was lost, but the data is still there.
// The legacy directory is skipped on purpose - finding it here would keep
// the well-known name forever instead of renaming it.
[[nodiscard]] QString LookupExisting(const QString &working) {
	const auto entries = QDir(working).entryList(
		QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
	for (const auto &entry : entries) {
		if (entry == QString::fromUtf8(kLegacyName)
			|| NeverData(entry)
			|| !ValidName(entry)
			|| !LooksLikeData(working + entry)) {
			continue;
		}
		return entry;
	}
	return QString();
}

[[nodiscard]] QString ReadPointer(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto name = QString::fromUtf8(file.read(1024)).trimmed();
	return ValidName(name) ? name : QString();
}

void WritePointer(const QString &path, const QString &name) {
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(name.toUtf8());
		file.close();
	} else {
		LOG(("App Error: Could not write '%1'.").arg(path));
	}
}

// Returns the name that should be used, 'tdata' if the rename failed.
[[nodiscard]] QString RenameLegacy(
		const QString &working,
		const QString &name) {
	const auto from = working + kLegacyName;
	const auto to = working + name;
	if (!QDir(from).exists() || QDir(to).exists()) {
		return name;
	} else if (QDir().rename(from, to)) {
		LOG(("App Info: Renamed the data directory to '%1'.").arg(name));
		return name;
	}
	LOG(("App Error: Could not rename '%1' to '%2', keeping the old name."
		).arg(from, to));
	return QString::fromUtf8(kLegacyName);
}

} // namespace

void InitDataDirectory() {
	const auto working = cWorkingDir();
	if (!GlobalName.isEmpty() && GlobalWorking == working) {
		return;
	}
	const auto pointer = working + kPointerName;

	auto name = ReadPointer(pointer);
	if (name.isEmpty()) {
		name = LookupExisting(working);
	}
	if (name.isEmpty()) {
		name = RenameLegacy(working, GenerateName());
	} else if (!QDir(working + name).exists()) {
		// The pointer survived, but the directory was renamed back by hand.
		name = RenameLegacy(working, name);
	}

	QDir().mkpath(working + name);
	if (name == QString::fromUtf8(kLegacyName)) {
		// The rename failed, so don't remember the legacy name - the next
		// launch should try to rename the directory again.
		QFile::remove(pointer);
	} else if (ReadPointer(pointer) != name) {
		WritePointer(pointer, name);
	}
	GlobalWorking = working;
	GlobalName = name;
}

const QString &DataDirName() {
	InitDataDirectory();
	return GlobalName;
}

QString DataPath() {
	return cWorkingDir() + DataDirName() + '/';
}

QString DataPath(const QString &relative) {
	return DataPath() + relative;
}

} // namespace Core
