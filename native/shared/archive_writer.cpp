// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "archive_writer.hpp"
#include "archives.hpp"
#include "digest.hpp"
#include "thread_name.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

namespace archives {
namespace {
// Begin and end commands hold no ring bytes, and a sparse file's data commands may hold one byte each;
// this caps them for archives of many empty files or tiny scattered blocks.
constexpr std::size_t max_commands = 4096, readback_block = 128 * 1024;
// Each queued file holds its open descriptor; this keeps the decoder from opening thousands ahead of a
// slow disk and running the process out of descriptors.
constexpr std::size_t max_queued_files = 32;
// Writer threads only hash and copy; the read-back buffer is on the heap.
constexpr std::size_t thread_stack = 256 * 1024;
}
Receipt::~Receipt() { close(); }

void Receipt::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool Receipt::open(const std::string& path, const std::string& header) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd_ < 0) return false;
  path_ = path;
  header_ = header.size();
  buffer_.reserve(buffer_bytes);
  buffer_ = header + "\n";
  return true;
}

bool Receipt::flush() {
  std::size_t offset = 0;
  while (!failed_ && offset < buffer_.size()) {
    const auto count = write(fd_, buffer_.data() + offset, buffer_.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) failed_ = true;
    else offset += count;
  }
  buffer_.clear();
  return !failed_ && fd_ >= 0;
}

bool Receipt::add(const std::string& digest, const std::string& name) {
  if (failed_ || fd_ < 0) return false;
  if (buffer_.size() + digest.size() + name.size() + 2 > buffer_bytes && !flush()) return false;
  buffer_ += digest;
  buffer_ += '|';
  buffer_ += name;
  buffer_ += '\n';
  ++files_;
  return true;
}

bool Receipt::set_header(const std::string& header) {
  if (failed_ || fd_ < 0 || header.size() != header_ || header.find('\n') != std::string::npos) return false;
  if (!flush()) return false;
  std::size_t offset = 0;
  while (offset < header.size()) {
    const auto count = pwrite(fd_, header.data() + offset, header.size() - offset, offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return !(failed_ = true);
    offset += count;
  }
  return true;
}

bool Receipt::commit() {
  bool ok = flush() && !fsync(fd_);
  if (fd_ >= 0 && ::close(fd_) && errno != EINTR) ok = false;
  fd_ = -1;
  return ok;
}

bool LineReader::next(std::string& line) {
  line.clear();
  while (!failed_) {
    const auto* begin = buffer_.data() + start_;
    if (const auto* newline = static_cast<const char*>(std::memchr(begin, '\n', end_ - start_))) {
      line.append(begin, newline - begin);
      start_ += newline - begin + 1;
      if (line.size() > longest_) break;
      return true;
    }
    line.append(begin, end_ - start_);
    start_ = end_ = 0;
    if (line.size() > longest_ || eof_) break;
    const auto count = read(fd_, buffer_.data(), buffer_.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) break;
    eof_ = !count;
    end_ = count;
  }
  // A last line without its newline, or one too long, makes the file unclean.
  failed_ = failed_ || !line.empty() || !eof_;
  return false;
}

// Waits have no timeout (no timed wait has been proven on the console): every change a waiter needs is
// notified. The job's cancel flag is not, so a waiter sees it at the next notification, which always
// comes while the other side has queued work: commands are sealed before any wait for room.

Writer::~Writer() {
  if (!ring_) return;
  aborted_ = true;
  stop_threads();
}

bool Writer::start(std::size_t ring_bytes, Receipt& receipt) {
  if (ring_bytes < 1024 * 1024) return false;
  receipt_ = &receipt;
  ring_ = static_cast<unsigned char*>(std::malloc(ring_bytes));
  if (!ring_) return false;
  capacity_ = ring_bytes;
  chunk_ = std::min<std::size_t>(4 * 1024 * 1024, capacity_ / 4);
  if (writer_.start([this] { write_loop(); }, thread_stack)) return true;
  std::free(ring_);
  ring_ = nullptr;
  return false;
}

void Writer::fail(const std::string& error) {
  std::lock_guard lock(mutex_);
  if (error_.empty()) error_ = error;
  failed_ = true;
  space_.notify_all();
  work_.notify_all();
}

// Empty on cancellation, as the inline path returns, so the job reports the cancel and not an error.
std::string Writer::error() {
  if (cancelled_) return {};
  std::lock_guard lock(mutex_);
  return error_.empty() && failed() ? "Extraction stopped." : error_;
}

bool Writer::push(Command command) {
  std::unique_lock lock(mutex_);
  if (!commands_.empty()) commands_.back().sealed = true;
  work_.notify_one();
  const bool file = command.kind == Kind::begin;
  space_.wait(lock, [&] { return (commands_.size() < max_commands && (!file || queued_files_ < max_queued_files)) || failed(); });
  if (failed()) {
    if (file) close(command.fd);
    return false;
  }
  queued_files_ += file;
  commands_.push_back(std::move(command));
  work_.notify_one();
  return true;
}

bool Writer::begin(int fd, std::string path, std::string name) {
  Command command{Kind::begin};
  command.fd = fd;
  command.path = std::move(path);
  command.name = std::move(name);
  return push(std::move(command));
}

bool Writer::end() { return push(Command{Kind::end}); }

bool Writer::append(const void* bytes, std::size_t size, std::uint64_t offset) {
  const auto* source = static_cast<const unsigned char*>(bytes);
  while (size) {
    const std::size_t piece = std::min(size, chunk_);
    std::unique_lock lock(mutex_);
    if (failed()) return false;
    const std::size_t position = head_ % capacity_;
    Command* last = commands_.empty() ? nullptr : &commands_.back();
    // Merges into the queued write when these bytes follow it both in the file and in the ring.
    const bool merge = last && last->kind == Kind::data && !last->sealed && last->offset + last->length == offset
        && last->length + piece <= chunk_ && last->ring + last->length == position
        && position + piece <= capacity_ && head_ - tail_ + piece <= capacity_;
    if (merge) {
      last->busy = true;
      head_ += piece;
      lock.unlock();
      std::memcpy(ring_ + position, source, piece);
      lock.lock();
      last->length += piece;
      last->busy = false;
      if (last->length == chunk_) last->sealed = true;
      work_.notify_one();
    } else {
      const std::size_t pad = position + piece > capacity_ ? capacity_ - position : 0;
      if (last) last->sealed = true;
      work_.notify_one();
      const auto room = [&] { return head_ - tail_ + pad + piece <= capacity_ && commands_.size() < max_commands; };
      if (!room()) {
        const auto waited = std::chrono::steady_clock::now();
        space_.wait(lock, [&] { return room() || failed(); });
        timings_.wait_us += elapsed_us(waited);
      }
      if (failed()) return false;
      Command command{Kind::data};
      command.ring = (head_ + pad) % capacity_;
      command.pad = pad;
      command.offset = offset;
      command.length = piece;
      command.busy = true;
      command.sealed = piece == chunk_;
      head_ += pad + piece;
      commands_.push_back(std::move(command));
      Command& queued = commands_.back();
      lock.unlock();
      std::memcpy(ring_ + queued.ring, source, piece);
      lock.lock();
      queued.busy = false;
      work_.notify_one();
    }
    source += piece;
    offset += piece;
    size -= piece;
  }
  return true;
}

void Writer::write_loop() {
  name_thread("archive-write");
  int fd = -1;
  std::string path, name;
  std::optional<integrity::Hash> hash;
  std::uint64_t hashed = 0, extent = 0;
  bool contiguous = true;
  while (true) {
    Command command;
    {
      std::unique_lock lock(mutex_);
      // A write still open to merging waits until the decoder seals it: a full chunk, the next file,
      // a wait for room, or the end.
      work_.wait(lock, [&] {
        if (commands_.empty()) return stopping_;
        const Command& front = commands_.front();
        return !front.busy && (front.sealed || stopping_ || failed());
      });
      if (commands_.empty()) {
        if (fd >= 0) close(fd);
        return;
      }
      command = std::move(commands_.front());
      commands_.pop_front();
      queued_files_ -= command.kind == Kind::begin;
      space_.notify_all();
    }
    const bool skip = failed();
    if (command.kind == Kind::begin) {
      if (fd >= 0) close(fd);
      fd = command.fd;
      path = std::move(command.path);
      name = std::move(command.name);
      hash.emplace();
      hashed = extent = 0;
      contiguous = true;
      if (skip) {
        close(fd);
        fd = -1;
      }
    } else if (command.kind == Kind::data) {
      const unsigned char* bytes = ring_ + command.ring;
      std::size_t written = 0;
      if (command.ring + command.length > capacity_) fail("Extraction write outside its buffer.");
      const auto writing = std::chrono::steady_clock::now();
      while (!skip && fd >= 0 && written < command.length && !failed()) {
        const auto count = pwrite(fd, bytes + written, command.length - written, command.offset + written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          fail(count < 0 ? std::strerror(errno) : "Short write.");
          break;
        }
        written += count;
      }
      timings_.write_us += elapsed_us(writing);
      ++timings_.writes;
      if (!failed() && fd >= 0) {
        if (contiguous && command.offset == hashed) {
          const auto hashing = std::chrono::steady_clock::now();
          hash->update(bytes, command.length);
          timings_.hash_us += elapsed_us(hashing);
          hashed += command.length;
        } else {
          contiguous = false;
        }
        extent = std::max<std::uint64_t>(extent, command.offset + command.length);
      }
      std::lock_guard lock(mutex_);
      tail_ += command.pad + command.length;
      space_.notify_all();
    } else if (fd >= 0) {
      std::string digest;
      if (!skip) {
        if (contiguous && hashed == extent) digest = hash->finish();
        else digest = readback(path);
      }
      if (!skip && digest.size() != 64) fail("Cannot verify extracted file.");
      if (!failed()) {
        if (!receipt_->add(digest, name)) fail("Cannot write extraction receipt.");
        const auto folder = path.size() - name.size() - 1;
        if (!path.ends_with("/" + name) || (!root_.empty() && path.compare(0, folder, root_)))
          fail("Extracted file outside its folder.");
        else if (root_.empty()) root_ = path.substr(0, folder);
      }
      if (close(fd) && errno != EINTR && !failed()) fail(std::strerror(errno));
      fd = -1;
    }
  }
}

std::string Writer::readback(const std::string& file) {
  const int fd = open(file.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return {};
  auto* buffer = static_cast<char*>(std::malloc(readback_block));
  integrity::Hash hash;
  bool ok = buffer;
  while (ok && !failed()) {
    const auto bytes = read(fd, buffer, readback_block);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) {
      ok = !bytes;
      break;
    }
    ok = hash.update(buffer, bytes);
  }
  std::free(buffer);
  close(fd);
  return ok && !failed() ? hash.finish() : std::string();
}

// Every file is on disk before the receipt: inputs are deleted once extraction completes.
void Writer::sync_loop(LineReader& lines, std::mutex& reading, std::atomic<std::size_t>& synced_files, std::size_t total, const Progress& progress) {
  name_thread("archive-sync");
  std::string line;
  while (!failed()) {
    {
      std::lock_guard lock(reading);
      if (!lines.next(line)) return;
    }
    const auto file = root_ + "/" + line.substr(line.find('|') + 1);
    const auto syncing = std::chrono::steady_clock::now();
    // fsync needs no write access; a filesystem that wants it gets the file opened for writing.
    int fd = open(file.c_str(), O_RDONLY | O_NOFOLLOW);
    bool synced = fd >= 0 && !fsync(fd);
    if (fd >= 0 && !synced && (errno == EBADF || errno == EINVAL)) {
      close(fd);
      fd = open(file.c_str(), O_WRONLY | O_NOFOLLOW);
      synced = fd >= 0 && !fsync(fd);
    }
    const int error = errno;
    if (fd >= 0) close(fd);
    timings_.sync_us += elapsed_us(syncing);
    ++timings_.syncs;
    if (!synced) fail(std::strerror(error));
    else if (const auto count = ++synced_files; progress) progress(count, total);
  }
}

void Writer::stop_threads() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    if (!commands_.empty()) commands_.back().sealed = true;
  }
  work_.notify_all();
  writer_.join();
  std::free(ring_);
  ring_ = nullptr;
}

std::string Writer::finish(const Progress& progress) {
  stop_threads();
  // The files to sync are read back from the receipt, past its header, a line at a time.
  int fd = -1;
  if (!failed() && (!receipt_->flush() || (fd = open(receipt_->path().c_str(), O_RDONLY | O_NOFOLLOW)) < 0))
    fail("Cannot read extraction receipt.");
  LineReader lines(fd);
  std::mutex reading;
  std::string header;
  if (fd >= 0 && !lines.next(header)) fail("Cannot read extraction receipt.");
  const std::size_t total = receipt_->files();
  std::atomic<std::size_t> synced{0};
  if (progress && !failed()) progress(0, total);
  // Without threads, the files are synced here, one after another.
  std::array<WorkerThread, syncers> threads;
  for (auto& thread : threads)
    if (!failed() && total > 1) thread.start([&] { sync_loop(lines, reading, synced, total, progress); }, thread_stack);
  sync_loop(lines, reading, synced, total, progress);
  for (auto& thread : threads) thread.join();
  if (fd >= 0) close(fd);
  if (cancelled_) return {};
  if (failed_ || aborted_) return error_.empty() ? "Extraction stopped." : error_;
  if (!lines.clean() || synced != total) return "Cannot read extraction receipt.";
  return {};
}
}
