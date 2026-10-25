/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mcp/mcp_settings.h"

#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"

namespace Mcp {
namespace {

constexpr auto kEnabledPref = "mcp/enabled";
constexpr auto kPortPref = "mcp/port";
constexpr auto kTokenPref = "mcp/token";
constexpr auto kCapabilitiesPref = "mcp/capabilities";

constexpr auto kTokenBytes = 32;

// Reading and drafting change nothing anyone else can see, and events only
// flow on subscriptions the client asks for. Anything acting as the user is
// left for the user to turn on.
constexpr auto kDefaultCapabilities = Capabilities()
	| Capability::Read
	| Capability::Drafts
	| Capability::Events;

[[nodiscard]] QByteArray GenerateToken() {
	auto bytes = QByteArray(kTokenBytes, Qt::Uninitialized);
	base::RandomFill(bytes.data(), bytes.size());
	return bytes.toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

[[nodiscard]] std::optional<quint64> ReadNumber(const char *key) {
	const auto stored = Core::App().settings().readPref<QByteArray>(key);
	auto ok = false;
	const auto value = stored.toULongLong(&ok);
	return ok ? std::make_optional(value) : std::nullopt;
}

} // namespace

Settings::Settings() {
	auto &settings = Core::App().settings();
	_enabled = settings.readPref<bool>(kEnabledPref, false);

	const auto port = ReadNumber(kPortPref).value_or(kDefaultPort);
	_port = (port >= kMinPort && port <= kMaxPort)
		? int(port)
		: kDefaultPort;

	const auto capabilities = ReadNumber(kCapabilitiesPref);
	_capabilities = capabilities
		? Capabilities::from_raw(Capabilities::Type(*capabilities))
		: kDefaultCapabilities;

	_token = settings.readPref<QByteArray>(kTokenPref);
	if (_token.current().isEmpty()) {
		_token = GenerateToken();
		save();
	}
}

bool Settings::enabled() const {
	return _enabled.current();
}

rpl::producer<bool> Settings::enabledValue() const {
	return _enabled.value();
}

void Settings::setEnabled(bool enabled) {
	if (_enabled.current() != enabled) {
		_enabled = enabled;
		save();
	}
}

int Settings::port() const {
	return _port.current();
}

rpl::producer<int> Settings::portValue() const {
	return _port.value();
}

void Settings::setPort(int port) {
	if (port >= kMinPort && port <= kMaxPort && _port.current() != port) {
		_port = port;
		save();
	}
}

QByteArray Settings::token() const {
	return _token.current();
}

rpl::producer<QByteArray> Settings::tokenValue() const {
	return _token.value();
}

void Settings::regenerateToken() {
	_token = GenerateToken();
	save();
}

Capabilities Settings::capabilities() const {
	return _capabilities.current();
}

rpl::producer<Capabilities> Settings::capabilitiesValue() const {
	return _capabilities.value();
}

bool Settings::has(Capability capability) const {
	return (_capabilities.current() & capability) != 0;
}

void Settings::setCapability(Capability capability, bool enabled) {
	const auto now = _capabilities.current();
	const auto updated = enabled ? (now | capability) : (now & ~capability);
	if (updated != now) {
		_capabilities = updated;
		save();
	}
}

void Settings::save() {
	auto &settings = Core::App().settings();
	settings.writePref<bool>(kEnabledPref, _enabled.current());
	settings.writePref<QByteArray>(
		kPortPref,
		QByteArray::number(_port.current()));
	settings.writePref<QByteArray>(kTokenPref, _token.current());
	settings.writePref<QByteArray>(
		kCapabilitiesPref,
		QByteArray::number(quint64(_capabilities.current().value())));
	Core::App().saveSettingsDelayed();
}

} // namespace Mcp
