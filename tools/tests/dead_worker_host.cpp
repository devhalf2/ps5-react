// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
//
// rar-extract as the console sees it once it has died: its socket reset, every read finds nothing and every
// write fails. A desktop child that dies reads as the end of its output instead. Each write goes to stderr.
#include "archives.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int archives::start_rar_worker(const std::string&, std::string&) { return 1; }
// A quarter second on the console; shorter here, so the extraction's silent-read limits pass quickly.
long archives::read_rar_worker(int, char*, std::size_t) {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return -1;
}
void archives::cancel_rar_worker(int) { std::cerr << "write cancel\n"; }
bool archives::tell_rar_worker(int, const std::string& line) {
  std::cerr << "write " << line;
  return false;
}
void archives::close_rar_worker(int) {}
