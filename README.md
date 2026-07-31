# KebabClient

A fork of [Telegram Desktop][telegram_desktop] that keeps local data encrypted at rest, and adds the things the official client leaves out.

Everything here is built on the stock facilities of tdesktop — the same local key, the same settings backend, the same message storage. There is no second database, no bundled SQLite, no forked `lib_ui`. That is deliberate: it keeps the diff against upstream small enough to rebase when a new version lands.

**[Download the latest release][releases]** — Windows x64.

## Local data

Out of the box, tdesktop encrypts `tdata` with a key that, without a passcode, is itself stored under an empty one. Copying the folder is then enough to take over the session, which is what every stealer does.

Here:

- **A local passcode is required.** It is asked on every launch and cannot be turned off. The key is derived with PBKDF2-HMAC-SHA512, 100 000 iterations.
- **The data directory has a random name.** A small `.data` pointer file records it; an existing `tdata` is moved in place on first launch. This alone stops nothing serious — malware can read the pointer too — but it does defeat the ones that blindly copy `<workdir>/tdata`.

Forget the passcode and the local data is unrecoverable. You will have to log in again.

## Modifications

All of it lives in its own **Modifications** section in Settings. Ghost mode and Streamer mode are also in the main side menu, next to Night mode.

**Ghost mode** — no read receipts, no typing status, no online status, no story views, no post view counts. Mentions and reactions are still reported to the server, otherwise their unread badge could never clear. A **Mark as read** entry in the chat context menu sends the receipt when you actually want to.

**Streamer mode** — windows are excluded from screen capture. Windows only; X11 and Wayland have no equivalent.

**Message history** — keeps what a message said before it was edited or deleted. Records are appended to one file encrypted under the same key as the rest of your data, so appending costs the same however much has piled up. Deleted messages stay on screen with a `deleted` badge and are recreated as local messages after a restart. Messages with a self-destruct timer are never stored.

**Allow copying and saving anywhere** — selecting text, *Save as*, downloads and the save button in the media viewer are refused by the client itself in chats that restrict saving, so this stops asking.

**Seconds in message time** — `12:34:56` instead of `12:34`.

## Getting content out of restricted chats

Worth being precise, because two different obstacles look the same from the outside.

Copying and saving are **client-side** checks. The setting above lifts them, and that is the whole story.

Forwarding is **server-side**. Telegram refuses `messages.forwardMessages` out of a chat flagged `noforwards`, and no client change reaches that. The same applies to a deleted message: there is no id left to forward.

So instead of a forward there is **Send as your own**, which rebuilds the message and sends it anew — the original text under a bold author line, all of it in a blockquote, with the media attached. It appears in place of *Forward* exactly when *Forward* would fail. Replying to a deleted message works the same way: the original is folded into your text as a quote and sent as an ordinary message.

## Other changes

- **No account limit.** Upstream allows three, plus one per logged-in Premium account, capped at six — a client-side cap the servers never enforced.
- **Peer ID and datacenter** are shown in profiles. Click either to copy. Bot callback data can be copied from the context menu.

## Not included

From [AyuGram][ayugram], whose feature set this borrows from and reimplements rather than copies:

- **Local Premium** — fakes a Premium badge locally. Useless, and arguably not legal.
- **Ayu Sync** — syncing settings through a third-party server.
- **Message shot** — screenshotting a message into an image.

## Known limits

- Restored deleted messages carry text only. Their media is not stored and is gone after a restart — the cache has no way to pin an entry, and that code lives in a submodule this fork cannot publish changes to.
- Reposting a *deleted* message with media is unverified. The file is sent by reference, and that reference belongs to a message the server no longer has. It does work for protected channels, where the message is still alive.
- Streamer mode does nothing outside Windows.
- Only Windows builds are published. The source builds on Linux and macOS the same way upstream does.

## Build instructions

Unchanged from upstream:

* [Windows (32-bit and 64-bit)][win]
* [macOS][mac]
* [GNU/Linux using Docker][linux]

You will need your own `api_id` / `api_hash` from [my.telegram.org][my_telegram]; see [docs/api_credentials.md][api_credentials].

## License

GPLv3 with the OpenSSL exception, same as upstream — the license is [here][license]. This is an unofficial fork, not affiliated with Telegram.

## Third-party

* Qt 6 ([LGPL](http://doc.qt.io/qt-6/lgpl.html)) and Qt 5.15 ([LGPL](http://doc.qt.io/qt-5/lgpl.html)) slightly patched
* OpenSSL 3.2.1 ([Apache License 2.0](https://openssl-library.org/source/license/apache-license-2.0.txt))
* WebRTC ([New BSD License](https://github.com/desktop-app/tg_owt/blob/master/LICENSE))
* zlib ([zlib License](http://www.zlib.net/zlib_license.html))
* LZMA SDK 9.20 ([public domain](http://www.7-zip.org/sdk.html))
* liblzma ([public domain](http://tukaani.org/xz/))
* Google Breakpad ([License](https://chromium.googlesource.com/breakpad/breakpad/+/master/LICENSE))
* Google Crashpad ([Apache License 2.0](https://chromium.googlesource.com/crashpad/crashpad/+/master/LICENSE))
* GYP ([BSD License](https://github.com/bnoordhuis/gyp/blob/master/LICENSE))
* Ninja ([Apache License 2.0](https://github.com/ninja-build/ninja/blob/master/COPYING))
* OpenAL Soft ([LGPL](https://github.com/kcat/openal-soft/blob/master/COPYING))
* Opus codec ([BSD License](http://www.opus-codec.org/license/))
* FFmpeg ([LGPL](https://www.ffmpeg.org/legal.html))
* Guideline Support Library ([MIT License](https://github.com/Microsoft/GSL/blob/master/LICENSE))
* Range-v3 ([Boost License](https://github.com/ericniebler/range-v3/blob/master/LICENSE.txt))
* Open Sans font ([Apache License 2.0](http://www.apache.org/licenses/LICENSE-2.0.html))
* Vazirmatn font ([SIL Open Font License 1.1](https://github.com/rastikerdar/vazirmatn/blob/master/OFL.txt))
* Emoji alpha codes ([MIT License](https://github.com/emojione/emojione/blob/master/extras/alpha-codes/LICENSE.md))
* xxHash ([BSD License](https://github.com/Cyan4973/xxHash/blob/dev/LICENSE))
* QR Code generator ([MIT License](https://github.com/nayuki/QR-Code-generator#license))
* CMake ([New BSD License](https://github.com/Kitware/CMake/blob/master/Copyright.txt))
* Hunspell ([LGPL](https://github.com/hunspell/hunspell/blob/master/COPYING.LESSER))
* Ada ([Apache License 2.0](https://github.com/ada-url/ada/blob/main/LICENSE-APACHE))

[//]: # (LINKS)
[telegram_desktop]: https://github.com/telegramdesktop/tdesktop
[ayugram]: https://github.com/AyuGram/AyuGramDesktop
[releases]: https://github.com/zxsrxt-kebab-case/tdesktop-mod/releases/latest
[license]: LICENSE
[win]: docs/building-win.md
[mac]: docs/building-mac.md
[linux]: docs/building-linux.md
[api_credentials]: docs/api_credentials.md
[my_telegram]: https://my.telegram.org
