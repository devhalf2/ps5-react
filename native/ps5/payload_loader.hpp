// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <string>

// Programs an app may not run itself (the console's install and launch services) go to the payload
// loader on 127.0.0.1:9021, which runs each with the rest of the connection as its standard input and
// output.
namespace payload_loader {
// Where send() finds a program named without a directory: /app0 unless set. The daemon, which outlives
// the app and its /app0, starts its workers from the copies in <FileSystem.dataDir>/bin.
void set_directory(const std::string& directory);
// Sends `program`, a file name in that directory or an absolute app path, then `request`. The connected
// socket, its sends and receives timing out after `timeout_us`; -1 with `error` (prefixed with `what`)
// when the program or the loader is not there.
int send(const char* program, const std::string& request, int timeout_us, const char* what, std::string& error);
// A socket connected to 127.0.0.1:port within five seconds, its sends and receives timing out after
// `timeout_us`; -1 when nothing listens there.
int connect(int port, int timeout_us);
int receive(int socket, char* data, std::size_t size);
bool write(int socket, const char* data, std::size_t size);
// Later receives wait at most `timeout_us`.
void receive_timeout(int socket, int timeout_us);
void close(int socket);
}
