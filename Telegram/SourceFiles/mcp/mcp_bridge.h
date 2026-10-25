/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Mcp {

// "Telegram -mcpbridge [port]" relays MCP between stdio and the running
// client's HTTP server: JSON-RPC lines from stdin are posted to it, its
// answers and its event stream go to stdout. Claude Code only listens for
// pushed channel events from servers it starts over stdio, so this is how
// subscriptions reach a conversation. The token comes from the
// KEBAB_MCP_TOKEN environment variable, never from the command line, where
// other processes could read it.
//
// It runs before anything of the app is set up and never starts it.
[[nodiscard]] bool IsBridgeInvocation(int argc, char *argv[]);
int RunBridge(int argc, char *argv[]);

} // namespace Mcp
