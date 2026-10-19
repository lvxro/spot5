// spot5 - Starting a payload through the ELF loader, and talking to it.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apps may not do some things payloads may (bind the mDNS port, listen, keep
// running in the background), so the app ships small payloads in assets/ and
// hands them to elfldr (127.0.0.1:9021), then talks to them over loopback.

#pragma once

#include <string>

namespace service
{

// Sends the ELF at `path` to elfldr, which starts it. Returns false and sets
// `error` (a message for the user, starting with `what`, e.g. "Pairing") if the
// file or the loader is missing.
bool send_to_elf_loader(const std::string &path, const std::string &what, std::string &error);

// Connects to a payload's loopback `port`; -1 if nothing listens there.
int connect_loopback(int port);

// Connects to the payload elfldr just started and waits for its "READY\n",
// trying for about `seconds`. Returns the connected socket (1 s receive
// timeout), or -1.
int open_ready_link(int port, int seconds);

} // namespace service
