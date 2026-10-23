// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include "worker_thread.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace archives {
// An extraction's completion receipt: a header line, then "<sha256>|<path>" for each file. Lines go to its
// file through a buffer of fixed size as files complete, so memory stays the same whatever the number of
// files; a game folder's receipt runs past any size worth holding whole. Used from one thread at a time.
class Receipt {
public:
  Receipt() = default;
  ~Receipt();
  Receipt(const Receipt&) = delete;
  Receipt& operator=(const Receipt&) = delete;
  // Creates the file, which must not exist, starting with `header`.
  bool open(const std::string& path, const std::string& header);
  bool add(const std::string& digest, const std::string& name);
  // Replaces the header with another of the same length.
  bool set_header(const std::string& header);
  // Writes out what is buffered, then fsyncs and closes the file.
  bool commit();
  // Writes out what is buffered, for reading the file back.
  bool flush();
  // Closes the file without writing what is buffered, before staging is removed.
  void close();
  const std::string& path() const { return path_; }
  std::size_t files() const { return files_; }

private:
  static constexpr std::size_t buffer_bytes = 64 * 1024;
  int fd_ = -1;
  std::string path_, buffer_;
  std::size_t header_ = 0, files_ = 0;
  bool failed_ = false;
};
// Reads a descriptor a line at a time through a buffer of fixed size. A line longer than `longest` or
// a read error ends the reading, and clean() then says false; so does a last line with no newline.
class LineReader {
public:
  explicit LineReader(int fd, std::size_t longest = 8192) : fd_(fd), longest_(longest) {}
  bool next(std::string& line);
  bool clean() const { return !failed_ && start_ == end_; }

private:
  int fd_;
  std::size_t longest_, start_ = 0, end_ = 0;
  std::array<char, 64 * 1024> buffer_{};
  bool failed_ = false, eof_ = false;
};
// Where an extraction's time went, for the console log. Each figure is summed over its threads, so
// sync time can exceed the wall time when several closers wait at once.
struct Timings {
  std::atomic<std::uint64_t> write_us{0}, hash_us{0}, sync_us{0}, wait_us{0}, writes{0}, syncs{0};
};
inline std::uint64_t elapsed_us(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count();
}
// Writes extracted files on two threads of its own so the decoder never waits on the disk. Decoded
// bytes are copied into one ring allocated at start(); consecutive blocks of a file are merged there
// into writes of up to `chunk` bytes. The writer thread pwrites, hashes and closes them. finish() then
// fsyncs every file, several at once: by then the system has written most of them back on its own, so
// each fsync has little left to wait for, where one per file while extracting stalled on each. Memory is fixed at start(): nothing grows while extracting except
// the command queue, which is capped. Every call is from the one decoding thread.
class Writer {
public:
  Writer(const std::atomic<bool>& cancelled, Timings& timings) : cancelled_(cancelled), timings_(timings) {}
  ~Writer();
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  // False when the ring or a thread cannot be had; the caller then writes inline. Each file's line goes
  // to `receipt` once it is written and hashed.
  bool start(std::size_t ring_bytes, Receipt& receipt);
  // Takes ownership of `fd` even on failure. `path` is the file's absolute path, `name` its receipt name.
  bool begin(int fd, std::string path, std::string name);
  bool append(const void* bytes, std::size_t size, std::uint64_t offset);
  bool end();
  // Waits until every file is written, hashed, synced and closed, and returns the first error (empty on
  // success or cancellation). The files are those the receipt names, read back from it. `progress` gets
  // the files synced of the total once the writes are done and after each fsync, from any of the
  // syncing threads.
  using Progress = std::function<void(std::size_t synced, std::size_t total)>;
  std::string finish(const Progress& progress = {});
  // The first error, or empty when the job was cancelled.
  std::string error();

private:
  enum class Kind { begin, data, end };
  struct Command {
    Command() = default;
    explicit Command(Kind type) : kind(type) {}
    Kind kind = Kind::end;
    int fd = -1;
    std::string path, name;
    std::uint64_t offset = 0, ring = 0;
    std::size_t length = 0, pad = 0;
    bool sealed = true, busy = false;
  };
  void write_loop();
  // fsyncs the receipt's files as `lines` reads them, one at a time, until none is left or one fails.
  void sync_loop(LineReader& lines, std::mutex& reading, std::atomic<std::size_t>& synced_files, std::size_t total, const Progress& progress);
  bool push(Command command);
  std::string readback(const std::string& file);
  bool failed() const { return failed_.load() || aborted_.load() || cancelled_.load(); }
  void fail(const std::string& error);
  void stop_threads();

  static constexpr std::size_t syncers = 4;
  const std::atomic<bool>& cancelled_;
  Timings& timings_;
  unsigned char* ring_ = nullptr;
  std::size_t capacity_ = 0, chunk_ = 0;
  std::uint64_t head_ = 0, tail_ = 0;
  std::size_t queued_files_ = 0;
  std::deque<Command> commands_;
  Receipt* receipt_ = nullptr;
  // The receipt's names are relative to this folder, taken from the first file.
  std::string error_, root_;
  std::mutex mutex_;
  std::condition_variable work_, space_;
  std::atomic<bool> failed_{false}, aborted_{false};
  bool stopping_ = false;
  WorkerThread writer_;
};
}
