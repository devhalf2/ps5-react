// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: MIT
//
// rar-extract's 7z extractor. Folders whose one coder is LZMA2, LZMA or a stored copy (what 7-Zip writes
// for games) are read with the public-domain LZMA SDK: LZMA2 on several threads whenever its stream holds
// independent blocks (7-Zip's own multithreaded compression writes them) and their memory fits, the others
// on one. An archive with any other folder (branch filters, PPMd, BCJ2, zstd, encryption) is extracted with
// libarchive, as before.
//
// Request, after the "7ZX1" line: "memory <bytes>" (what the engine's preflight says the decoder needs, 1
// when it could not read the headers), "cap <bytes>" (the most this process may hold for decoding),
// "threads <n>", "limit <bytes>", "flatten <extension>"..., "reserved <text>", "dest <directory>",
// "source <path>"... in volume order, "wait 1" when the one source is still downloading, then "end".
// The engine can then write "cancel" at any time, and for a downloading source "have <prefix> <tail>
// <total>" (its first `prefix` bytes and every byte from `tail` to `total` are on disk) and "done <total>"
// once the download is complete. Answers as for a RAR set: "ready", "d <bytes>" once the headers are read,
// "p <bytes written>", "k" every second, "f <sha256> <size> <path>" once a file is on disk and synced, "s"
// once every file is decoded, "t <key>=<value>...", then "ok", "cancelled" or "fail <reason>".
//
// A downloading 7z is extracted as it arrives: its headers sit at its end, which the engine downloads
// first, and its folders are decoded as their bytes come in. One that needs libarchive fails with
// `stream_refused` before anything is written, and the app extracts it once the download is complete.
//
// A program's malloc on the console has no ceiling of its own: the system hands over memory it takes
// from everything else, slowly (a probe on a console took 8.5 s for 512 MiB and froze its interface
// beyond that), and keeps it with this process until it exits. So the decoder's memory is taken and
// every page touched before anything is decoded; when the console needs over a second to give it, the
// extraction stops there instead of running with the console short. The block is then freed into this
// process's heap, where the decoder's own allocations find it.
#include "sevenzip.hpp"
#include "worker_io.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include "7z.h"
#include "7zCrc.h"
#include "Alloc.h"
#include "Lzma2DecMt.h"
#include "LzmaDec.h"

namespace sevenzip {
namespace {
using namespace worker;
const char* memory_busy = "The console has too little free memory to extract this archive now. Close other apps and retry.";
const char* unsafe = "Archive contains an unsafe path, link or special file.";
const char* too_large = "Archive exceeds the extraction size limit.";
const char* damaged = "Unsupported or damaged 7z archive.";
const char* checksum = "7z checksum mismatch; the archive is damaged.";
const char* incomplete = "Incomplete 7z archive.";
const char* stream_refused = "This 7z can't be extracted while it downloads.";
// What the extraction holds beside the decoder: the writer's buffers and the read buffers.
constexpr std::uint64_t overhead = 32ULL << 20;
constexpr std::uint64_t claim_ms = 1000;
constexpr std::uint64_t page = 4096;
constexpr size_t read_block = 1 << 20;
// No header allocation past this: the engine's preflight allows 16 MiB of headers.
constexpr size_t max_header_allocation = 64 << 20;
constexpr std::uint32_t method_copy = 0, method_lzma2 = 0x21, method_lzma = 0x030101;
constexpr std::uint32_t unix_extension = 0x8000, no_folder = std::uint32_t(-1);
constexpr std::uint64_t max_files = 100000;

struct Options {
  std::uint64_t memory = 0, cap = 0, limit = 0;
  unsigned threads = 1;
  bool wait = false;
  std::vector<std::string> flatten, sources;
  std::string reserved, destination;
};

// Set by the control thread (a cancel, or the engine gone).
std::atomic<bool> cancelled{false};
bool stopping() { return cancelled || output_lost; }
// A downloading source, as the engine last reported it.
std::atomic<std::uint64_t> grown_prefix{0}, grown_tail{~0ULL}, grown_total{0};
std::atomic<bool> grown_whole{false};

bool read_options(Options& options) {
  std::string line;
  while (read_line(line)) {
    const auto space = line.find(' ');
    const std::string key = line.substr(0, space), value = space == std::string::npos ? "" : line.substr(space + 1);
    if (key == "end") {
      if (!options.cap) options.cap = options.memory;
      return !options.destination.empty() && !options.sources.empty() && options.limit && options.memory &&
             (!options.wait || options.sources.size() == 1);
    }
    if (key == "memory") options.memory = strtoull(value.c_str(), nullptr, 10);
    else if (key == "cap") options.cap = strtoull(value.c_str(), nullptr, 10);
    else if (key == "threads") options.threads = std::max(1u, unsigned(strtoul(value.c_str(), nullptr, 10)));
    else if (key == "limit") options.limit = strtoull(value.c_str(), nullptr, 10);
    else if (key == "wait") options.wait = value == "1";
    else if (key == "flatten") options.flatten.push_back(value);
    else if (key == "reserved") options.reserved = value;
    else if (key == "dest") options.destination = value;
    else if (key == "source") options.sources.push_back(value);
  }
  return false;
}

// Reads the engine's lines after the request until it says cancel or goes away. Detached: it may still be
// waiting for a line when the extraction ends.
void control() {
  std::string line;
  while (read_line(line)) {
    if (line == "cancel") break;
    unsigned long long a = 0, b = 0, c = 0;
    if (std::sscanf(line.c_str(), "have %llu %llu %llu", &a, &b, &c) == 3) {
      grown_total = c;
      grown_tail = b;
      grown_prefix = a;
    } else if (std::sscanf(line.c_str(), "done %llu", &a) == 1) {
      grown_total = a;
      grown_prefix = a;
      grown_whole = true;
    }
  }
  cancelled = true;
}

// Takes `bytes` and writes a byte in every page so the console really hands them over, then frees them.
std::string claim(std::uint64_t bytes, std::uint64_t& took_ms) {
  const auto started = now_ns();
  auto* block = static_cast<volatile unsigned char*>(std::malloc(bytes));
  if (!block) return too_much_memory;
  for (std::uint64_t at = 0; at < bytes; at += page) block[at] = 1;
  took_ms = (now_ns() - started) / 1000000;
  std::free(const_cast<unsigned char*>(block));
  return took_ms > claim_ms ? memory_busy : std::string();
}

struct Progress {
  std::uint64_t written = 0, last_report = 0;
  void beat() {
    const auto now = now_ns();
    if (now - last_report < progress_ns) return;
    last_report = now;
    say("p " + std::to_string(written));
  }
};

// The sources as one byte range: a 7z split into .001, .002... volumes is one archive cut into pieces. A
// downloading source is read as the engine reports its bytes on disk.
class Input {
public:
  ~Input() {
    for (auto& volume : volumes_)
      if (volume.fd >= 0) close(volume.fd);
  }
  bool open(const Options& options, std::string& error) {
    growing_ = options.wait;
    std::uint64_t start = 0;
    for (const auto& path : options.sources) {
      Volume volume{-1, start, 0, path};
      if (!growing_) {
        struct stat st;
        volume.fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (volume.fd < 0 || fstat(volume.fd, &st) || !S_ISREG(st.st_mode)) {
          if (volume.fd >= 0) close(volume.fd);
          error = "Archive source is missing or not a regular file.";
          return false;
        }
        volume.size = std::uint64_t(st.st_size);
        start += volume.size;
      }
      volumes_.push_back(volume);
    }
    size_ = start;
    return true;
  }
  // The archive's size; a downloading one's once the engine said it.
  bool size(std::uint64_t& bytes) {
    while (growing_ && !grown_total)
      if (!pause()) return false;
    bytes = growing_ ? grown_total.load() : size_;
    return true;
  }
  // Up to `size` bytes at `at` into `data`, `got` of them (0 at the end); false on a read error or a stop.
  bool read(std::uint64_t at, void* data, size_t size, size_t& got) {
    got = 0;
    if (growing_ ? !available(at, size) : false) return false;
    if (!growing_) size = at >= size_ ? 0 : size_t(std::min<std::uint64_t>(size, size_ - at));
    auto* out = static_cast<unsigned char*>(data);
    while (got < size) {
      auto volume = std::upper_bound(volumes_.begin(), volumes_.end(), at, [](std::uint64_t offset, const Volume& v) { return offset < v.start; });
      if (volume == volumes_.begin()) return false;
      --volume;
      if (volume->fd < 0 && !open_growing(*volume)) return false;
      const std::uint64_t inside = at - volume->start;
      const size_t want = growing_ ? size - got : size_t(std::min<std::uint64_t>(size - got, volume->size - inside));
      const ssize_t count = pread(volume->fd, out + got, want, off_t(inside));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      got += size_t(count);
      at += std::uint64_t(count);
    }
    return true;
  }
  bool growing() const { return growing_; }
  std::uint64_t waited_ns = 0;

private:
  struct Volume {
    int fd;
    std::uint64_t start, size;
    std::string path;
  };
  bool pause() {
    if (stopping()) return false;
    const auto started = now_ns();
    usleep(20000);
    waited_ns += now_ns() - started;
    return !stopping();
  }
  // Waits until bytes from `at` are on disk, and trims `size` to what is.
  bool available(std::uint64_t at, size_t& size) {
    while (true) {
      const bool whole = grown_whole;
      const std::uint64_t total = grown_total, prefix = grown_prefix, tail = grown_tail;
      if (total) {
        if (at >= total) {
          size = 0;
          return true;
        }
        size = size_t(std::min<std::uint64_t>(size, total - at));
      }
      if (whole || (total && at >= tail)) return true;
      if (at < prefix) {
        size = size_t(std::min<std::uint64_t>(size, prefix - at));
        return true;
      }
      if (!pause()) return false;
    }
  }
  // A download is written beside its final name until it completes; the descriptor follows the rename.
  bool open_growing(Volume& volume) {
    for (const auto& path : {volume.path, volume.path + ".part"}) {
      struct stat st;
      volume.fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
      if (volume.fd >= 0 && !fstat(volume.fd, &st) && S_ISREG(st.st_mode)) return true;
      if (volume.fd >= 0) close(volume.fd);
      volume.fd = -1;
    }
    return false;
  }
  std::vector<Volume> volumes_;
  std::uint64_t size_ = 0;
  bool growing_ = false;
};

// The SDK's interfaces sit first in these structs, and are handed back as const pointers.
template <class Outer, class Interface> Outer* outer(const Interface* p) { return reinterpret_cast<Outer*>(const_cast<Interface*>(p)); }

// The LZMA SDK's view of the input: a seekable stream for the headers, a bounded one for a folder's data.
struct SeekStream {
  ISeekInStream vt;
  Input* input;
  std::uint64_t at;
};
SRes seek_read(ISeekInStreamPtr p, void* data, size_t* size) {
  auto* self = outer<SeekStream>(p);
  size_t got = 0;
  if (!self->input->read(self->at, data, *size, got)) return stopping() ? SZ_ERROR_PROGRESS : SZ_ERROR_READ;
  self->at += got;
  *size = got;
  return SZ_OK;
}
SRes seek_seek(ISeekInStreamPtr p, Int64* position, ESzSeek origin) {
  auto* self = outer<SeekStream>(p);
  std::uint64_t base = 0;
  if (origin == SZ_SEEK_CUR) base = self->at;
  else if (origin == SZ_SEEK_END && !self->input->size(base)) return stopping() ? SZ_ERROR_PROGRESS : SZ_ERROR_READ;
  const auto target = Int64(base) + *position;
  if (target < 0) return SZ_ERROR_PARAM;
  self->at = std::uint64_t(target);
  *position = target;
  return SZ_OK;
}
struct RangeStream {
  ISeqInStream vt;
  Input* input;
  std::uint64_t at, end;
};
SRes range_read(ISeqInStreamPtr p, void* data, size_t* size) {
  auto* self = outer<RangeStream>(p);
  const size_t want = size_t(std::min<std::uint64_t>(*size, self->end - self->at));
  size_t got = 0;
  if (want && !self->input->read(self->at, data, want, got)) return stopping() ? SZ_ERROR_PROGRESS : SZ_ERROR_READ;
  if (want && !got) return SZ_ERROR_INPUT_EOF;
  self->at += got;
  *size = got;
  return SZ_OK;
}

// The header's allocations, each refused past what a sane header needs.
void* header_alloc(ISzAllocPtr, size_t size) { return size > max_header_allocation ? nullptr : std::malloc(size); }
void header_free(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc header_allocator = {header_alloc, header_free};

std::uint64_t lzma2_dictionary(Byte property) {
  if (property > 40) return ~0ULL;
  return property == 40 ? 0xFFFFFFFFULL : (std::uint64_t(2 | (property & 1)) << (property / 2 + 11));
}

struct Folder {
  std::uint32_t method = 0;
  const Byte* properties = nullptr;
  unsigned property_size = 0;
  std::uint64_t pack_position = 0, pack_size = 0, unpack_size = 0;
  bool crc_defined = false;
  std::uint32_t crc = 0;
};
// A folder this file decodes itself: one coder of a known method, reading one packed stream.
bool simple_folder(const CSzArEx& db, std::uint32_t index, Folder& folder) {
  CSzFolder parsed;
  const Byte* coders = db.db.CodersData + db.db.FoCodersOffsets[index];
  CSzData data{coders, db.db.FoCodersOffsets[index + 1] - db.db.FoCodersOffsets[index]};
  if (SzGetNextFolderItem(&parsed, &data) != SZ_OK || parsed.NumCoders != 1 || parsed.NumPackStreams != 1) return false;
  const auto& coder = parsed.Coders[0];
  folder.method = coder.MethodID;
  folder.properties = coders + coder.PropsOffset;
  folder.property_size = coder.PropsSize;
  if (coder.NumStreams != 1) return false;
  if (folder.method == method_lzma2 ? folder.property_size != 1
      : folder.method == method_lzma ? folder.property_size != 5
      : folder.method != method_copy)
    return false;
  const auto first = db.db.FoStartPackStreamIndex[index];
  folder.pack_position = db.dataPos + db.db.PackPositions[first];
  folder.pack_size = db.db.PackPositions[first + 1] - db.db.PackPositions[first];
  folder.unpack_size = SzAr_GetFolderUnpackSize(&db.db, index);
  folder.crc_defined = SzBitWithVals_Check(&db.db.FolderCRCs, index);
  if (folder.crc_defined) folder.crc = db.db.FolderCRCs.Vals[index];
  return true;
}
std::uint64_t folder_dictionary(const Folder& folder) {
  if (folder.method == method_lzma2) return lzma2_dictionary(folder.properties[0]);
  if (folder.method == method_lzma)
    return std::uint64_t(folder.properties[1]) | std::uint64_t(folder.properties[2]) << 8 | std::uint64_t(folder.properties[3]) << 16 |
           std::uint64_t(folder.properties[4]) << 24;
  return 0;
}

// The unpacked size of an LZMA2 stream's first block, read from its chunk headers alone: up to its second
// dictionary reset. 0 when there is none within `limit` bytes, so the stream decodes on one thread.
std::uint64_t first_block(Input& input, const Folder& folder, std::uint64_t limit) {
  std::uint64_t at = folder.pack_position, unpacked = 0;
  const std::uint64_t end = folder.pack_position + folder.pack_size;
  bool started = false;
  while (at < end && unpacked <= limit) {
    unsigned char head[6];
    size_t got = 0;
    if (!input.read(at, head, size_t(std::min<std::uint64_t>(sizeof head, end - at)), got) || !got) return 0;
    const unsigned control = head[0];
    bool reset;
    std::uint64_t unpack, skip;
    if ((control == 1 || control == 2) && got >= 3) {
      reset = control == 1;
      unpack = (std::uint64_t(head[1]) << 8 | head[2]) + 1;
      skip = 3 + unpack;
    } else if (control >= 0x80 && got >= 5) {
      const unsigned mode = (control >> 5) & 3;
      reset = mode == 3;
      unpack = (std::uint64_t(control & 0x1F) << 16 | std::uint64_t(head[1]) << 8 | head[2]) + 1;
      skip = 5 + (mode >= 2) + ((std::uint64_t(head[3]) << 8 | head[4]) + 1);
    } else {
      return 0;
    }
    if (reset && started) return unpacked;
    started = true;
    unpacked += unpack;
    at += skip;
  }
  return 0;
}

// How one folder decodes: LZMA2 on `threads` threads with blocks of up to `block` bytes, else on one.
struct Plan {
  unsigned threads = 1;
  std::uint64_t block = 0, memory = 0;
};
// A thread holds a block's output and its packed input (measured at 1.6 times the block for a 128 MiB one).
// The dictionary stays reserved beside them: the decoder falls back to it for a block larger than planned.
Plan plan(Input& input, const Folder& folder, unsigned threads, std::uint64_t room) {
  Plan result;
  result.memory = std::min(folder_dictionary(folder), folder.unpack_size + (1 << 20));
  if (folder.method != method_lzma2 || threads < 2 || room <= result.memory) return result;
  const std::uint64_t left = room - result.memory;
  const std::uint64_t block = first_block(input, folder, left / 2);
  if (!block || block >= folder.unpack_size) return result;
  const std::uint64_t rounded = (block + (1 << 20) - 1) & ~std::uint64_t((1 << 20) - 1);
  const double ratio = folder.unpack_size ? double(folder.pack_size) / double(folder.unpack_size) : 1;
  const std::uint64_t each = rounded + std::uint64_t(double(rounded) * std::min(ratio + 0.1, 1.07)) + (2 << 20);
  const auto fit = unsigned(std::min<std::uint64_t>(threads, left / each));
  if (fit < 2) return result;
  result.threads = fit;
  result.block = rounded;
  result.memory += std::uint64_t(fit) * each;
  return result;
}

// UTF-16 names as UTF-8; false for a broken surrogate pair.
bool utf8_name(const std::vector<UInt16>& name, std::string& out) {
  out.clear();
  for (size_t i = 0; i < name.size() && name[i]; ++i) {
    std::uint32_t c = name[i];
    if (c >= 0xD800 && c < 0xDC00) {
      if (i + 1 >= name.size() || name[i + 1] < 0xDC00 || name[i + 1] >= 0xE000) return false;
      c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
    } else if (c >= 0xDC00 && c < 0xE000) {
      return false;
    }
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | c >> 12);
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | c >> 18);
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return true;
}

// The rules every entry passes, whichever reader found it: a safe relative name outside the engine's
// reserved names, within the size limit, created without following links. A file's descriptor is
// returned in `fd` and its name in `stored`: images are published at the set root.
std::string create_entry(const Options& options, std::string name, bool directory, bool executable, std::uint64_t size,
                         std::uint64_t& declared, int& fd, std::string& stored) {
  fd = -1;
  while (!name.empty() && name.back() == '/') name.pop_back();
  if (!name.empty() && name[0] != '/') name = without_dots(name);
  if (!safe(name) || (!options.reserved.empty() && name.find(options.reserved) != std::string::npos)) return unsafe;
  if (!directory) {
    if (size > options.limit - declared) return too_large;
    declared += size;
    // ShadowMount's default scan reaches one subdirectory, so images are published at the set root.
    if (flattened(options.flatten, name)) name = name.substr(name.rfind('/') + 1);
  }
  std::string target;
  if (!make_parents(options.destination, name, target)) return strerror(errno);
  if (directory) {
    if (!make_directory(target) && (errno != EEXIST || !plain_directory(target))) return strerror(errno);
    return {};
  }
  // The engine's mode rule (worker_io.hpp): programs by name, or marked executable in the archive.
  const mode_t mode = program_name(name) || executable ? 0755 : 0644;
  fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
  if (fd < 0) return strerror(errno);
  if (fchmod(fd, mode)) {
    const std::string error = strerror(errno);
    close(fd);
    fd = -1;
    return error;
  }
  stored = name;
  return {};
}

// Writes the 7z's entries in archive order as their folders decode: directories and empty files as the
// order reaches them, each file with data as its folder's output covers it, its CRC checked at its end.
class Files {
public:
  Files(const CSzArEx& db, const Options& options, Writer& writer, Progress& progress)
      : db_(db), options_(options), writer_(writer), progress_(progress) {}
  std::string error;

  // Creates every entry before the next file with data, which must be in `folder`; with `no_folder`,
  // every entry left, of which none may have data.
  bool reach(std::uint32_t folder) {
    for (; error.empty() && next_ < db_.NumFiles; ++next_) {
      const std::uint32_t owner = db_.FileToFolder[next_];
      if (!SzArEx_IsDir(&db_, next_) && owner != no_folder && SzArEx_GetFileSize(&db_, next_)) {
        if (owner == folder) return true;
        // Files with data come in their folders' order: anything else is a damaged index.
        error = damaged;
        return false;
      }
      if (!entry(next_, 0) || (!SzArEx_IsDir(&db_, next_) && !close_file())) return false;
    }
    if (error.empty() && folder != no_folder) error = damaged;
    return error.empty();
  }
  // Takes a folder's decoded bytes, in order.
  bool consume(std::uint32_t folder, const unsigned char* data, size_t size) {
    while (size) {
      if (stopping()) return false;
      if (!open_ && (!reach(folder) || !entry(next_, SzArEx_GetFileSize(&db_, next_)))) return false;
      const size_t count = size_t(std::min<std::uint64_t>(size, remaining_));
      if (count > options_.limit - progress_.written) {
        error = too_large;
        return false;
      }
      if (!writer_.append(data, count)) {
        error = writer_.error();
        return false;
      }
      crc_ = CrcUpdate(crc_, data, count);
      remaining_ -= count;
      progress_.written += count;
      progress_.beat();
      data += count;
      size -= count;
      if (!remaining_) {
        if (!close_file()) return false;
        ++next_;
      }
    }
    return true;
  }
  bool idle() const { return !open_; }

private:
  // Creates entry `index`; a file stays open for `consume` or close_file().
  bool entry(std::uint32_t index, std::uint64_t size) {
    std::vector<UInt16> wide(SzArEx_GetFileNameUtf16(&db_, index, nullptr));
    SzArEx_GetFileNameUtf16(&db_, index, wide.data());
    std::string name;
    if (!utf8_name(wide, name)) {
      error = "Archive has a file name this console cannot read.";
      return false;
    }
    bool executable = false;
    if (SzBitWithVals_Check(&db_.Attribs, index) && (db_.Attribs.Vals[index] & unix_extension)) {
      const auto mode = db_.Attribs.Vals[index] >> 16;
      const auto type = mode & S_IFMT;
      // Links and special files are refused, as libarchive's entries are.
      if (type && type != S_IFREG && type != S_IFDIR) {
        error = unsafe;
        return false;
      }
      executable = mode & 0111;
    }
    const bool directory = SzArEx_IsDir(&db_, index);
    int fd = -1;
    std::string stored;
    error = create_entry(options_, name, directory, executable, size, declared_, fd, stored);
    if (!error.empty() || directory) return error.empty();
    writer_.begin(fd, stored);
    file_ = index;
    size_ = remaining_ = size;
    crc_ = CRC_INIT_VAL;
    open_ = true;
    return true;
  }
  bool close_file() {
    open_ = false;
    if (SzBitWithVals_Check(&db_.CRCs, file_) && CRC_GET_DIGEST(crc_) != db_.CRCs.Vals[file_]) {
      error = checksum;
      return false;
    }
    writer_.end(size_);
    return true;
  }

  const CSzArEx& db_;
  const Options& options_;
  Writer& writer_;
  Progress& progress_;
  std::uint32_t next_ = 0, file_ = 0;
  std::uint64_t size_ = 0, remaining_ = 0, declared_ = 0;
  std::uint32_t crc_ = 0;
  bool open_ = false;
};

struct Sink {
  ISeqOutStream vt;
  Files* files;
  std::uint32_t folder;
  std::uint32_t crc;
};
size_t sink_write(ISeqOutStreamPtr p, const void* data, size_t size) {
  auto* self = outer<Sink>(p);
  self->crc = CrcUpdate(self->crc, data, size);
  return self->files->consume(self->folder, static_cast<const unsigned char*>(data), size) ? size : 0;
}
SRes stop_progress(ICompressProgressPtr, UInt64, UInt64) { return stopping() ? SZ_ERROR_PROGRESS : SZ_OK; }

// What a failed decode means, once the files' own errors are taken.
std::string decode_error(SRes result, const Files& files) {
  if (!files.error.empty()) return files.error;
  if (result == SZ_ERROR_MEM) return too_much_memory;
  if (result == SZ_ERROR_INPUT_EOF || result == SZ_ERROR_READ) return incomplete;
  return damaged;
}

// Decodes folder `index` into the files as `run` plans.
std::string decode(Input& input, std::uint32_t index, const Folder& folder, const Plan& run, Files& files) {
  if (!files.reach(index)) return files.error;
  Sink sink{{sink_write}, &files, index, CRC_INIT_VAL};
  RangeStream range{{range_read}, &input, folder.pack_position, folder.pack_position + folder.pack_size};
  ICompressProgress stop{stop_progress};
  SRes result = SZ_OK;
  if (folder.method == method_copy) {
    if (folder.pack_size != folder.unpack_size) return damaged;
    std::vector<unsigned char> buffer(read_block);
    for (std::uint64_t left = folder.unpack_size; left && result == SZ_OK;) {
      size_t size = size_t(std::min<std::uint64_t>(left, buffer.size()));
      result = range_read(&range.vt, buffer.data(), &size);
      if (result == SZ_OK && !size) result = SZ_ERROR_INPUT_EOF;
      if (result == SZ_OK && sink_write(&sink.vt, buffer.data(), size) != size) result = SZ_ERROR_WRITE;
      left -= size;
    }
  } else if (folder.method == method_lzma2) {
    CLzma2DecMtProps properties;
    Lzma2DecMtProps_Init(&properties);
    properties.inBufSize_ST = read_block;
    properties.outStep_ST = read_block;
    properties.numThreads = run.threads;
    properties.inBufSize_MT = read_block;
    if (run.threads > 1) {
      properties.outBlockMax = size_t(run.block);
      properties.inBlockMax = size_t(run.block + run.block / 16);
    }
    CLzma2DecMtHandle decoder = Lzma2DecMt_Create(&g_AlignedAlloc, &g_MidAlloc);
    if (!decoder) return too_much_memory;
    const UInt64 size = folder.unpack_size;
    UInt64 consumed = 0;
    int threaded = 0;
    result = Lzma2DecMt_Decode(decoder, folder.properties[0], &properties, &sink.vt, &size, 1, &range.vt, &consumed, &threaded, &stop);
    Lzma2DecMt_Destroy(decoder);
    if (result == SZ_OK && consumed != folder.pack_size) result = SZ_ERROR_DATA;
  } else {
    CLzmaDec decoder;
    LzmaDec_CONSTRUCT(&decoder);
    result = LzmaDec_Allocate(&decoder, folder.properties, folder.property_size, &g_Alloc);
    if (result != SZ_OK) return result == SZ_ERROR_MEM ? too_much_memory : damaged;
    LzmaDec_Init(&decoder);
    std::vector<unsigned char> in(read_block), out(4 * read_block);
    size_t in_at = 0, in_size = 0;
    for (std::uint64_t left = folder.unpack_size; result == SZ_OK && left && !stopping();) {
      if (in_at == in_size) {
        in_at = 0;
        in_size = in.size();
        result = range_read(&range.vt, in.data(), &in_size);
        if (result != SZ_OK) break;
      }
      SizeT out_size = SizeT(std::min<std::uint64_t>(left, out.size())), in_used = in_size - in_at;
      ELzmaStatus status;
      result = LzmaDec_DecodeToBuf(&decoder, out.data(), &out_size, in.data() + in_at, &in_used,
                                   out_size == left ? LZMA_FINISH_END : LZMA_FINISH_ANY, &status);
      in_at += in_used;
      if (result != SZ_OK) break;
      if (!out_size && !in_used) result = in_size ? SZ_ERROR_DATA : SZ_ERROR_INPUT_EOF;
      else if (out_size && sink_write(&sink.vt, out.data(), out_size) != out_size) result = SZ_ERROR_WRITE;
      left -= out_size;
    }
    LzmaDec_Free(&decoder, &g_Alloc);
  }
  if (stopping()) return {};
  if (result != SZ_OK) return decode_error(result, files);
  if (!files.idle()) return damaged;
  if (folder.crc_defined && CRC_GET_DIGEST(sink.crc) != folder.crc) return checksum;
  return {};
}

struct Report {
  unsigned threads = 1;
  std::uint64_t block = 0, claim_ms = 0;
  bool sdk = false;
};

// The LZMA SDK's extraction. Sets `fallback`, with nothing written, for an archive with a folder it does
// not decode or headers it cannot read: libarchive tries it then, unless the archive is still downloading.
std::string extract_sdk(const Options& options, Input& input, Writer& writer, Progress& progress, Report& report, bool& fallback) {
  fallback = false;
  CSzArEx db;
  SzArEx_Init(&db);
  struct Free {
    CSzArEx& db;
    ~Free() { SzArEx_Free(&db, &header_allocator); }
  } free_db{db};
  SeekStream seek{{seek_read, seek_seek}, &input, 0};
  CLookToRead2 look;
  LookToRead2_CreateVTable(&look, False);
  std::vector<Byte> look_buffer(1 << 16);
  look.buf = look_buffer.data();
  look.bufSize = look_buffer.size();
  look.realStream = &seek.vt;
  LookToRead2_INIT(&look);
  const SRes opened = SzArEx_Open(&db, &look.vt, &header_allocator, &header_allocator);
  if (stopping()) return {};
  std::vector<Folder> folders(opened == SZ_OK ? db.db.NumFolders : 0);
  bool simple = opened == SZ_OK;
  for (std::uint32_t i = 0; simple && i < folders.size(); ++i) simple = simple_folder(db, i, folders[i]);
  if (!simple) {
    // Encrypted headers, a method this reader does not know or headers it refuses: libarchive's to judge.
    if (input.growing()) return opened == SZ_ERROR_INPUT_EOF || opened == SZ_ERROR_READ ? incomplete : stream_refused;
    fallback = true;
    return {};
  }
  report.sdk = true;
  if (db.NumFiles > max_files) return "Archive has too many entries.";
  std::uint64_t total = 0;
  for (std::uint32_t i = 0; i < db.NumFiles; ++i) {
    if (SzArEx_IsDir(&db, i)) continue;
    const auto size = SzArEx_GetFileSize(&db, i);
    if (size > options.limit - total) return too_large;
    total += size;
  }
  say("d " + std::to_string(total));
  // Each folder's plan; the memory the largest needs is claimed before anything is written.
  const std::uint64_t room = options.cap > overhead ? options.cap - overhead : 0;
  std::vector<Plan> plans(folders.size());
  std::uint64_t memory = 1 << 20;
  for (size_t i = 0; i < folders.size(); ++i) {
    if (folder_dictionary(folders[i]) > room) return too_much_memory;
    plans[i] = plan(input, folders[i], options.threads, room);
    if (stopping()) return {};
    memory = std::max(memory, plans[i].memory);
    if (plans[i].threads > report.threads) {
      report.threads = plans[i].threads;
      report.block = plans[i].block;
    }
  }
  if (const auto refused = claim(memory + overhead, report.claim_ms); !refused.empty()) return refused;
  Files files(db, options, writer, progress);
  for (std::uint32_t i = 0; i < folders.size() && !stopping(); ++i)
    if (const auto error = decode(input, i, folders[i], plans[i], files); !error.empty()) return error;
  if (stopping() || !files.reach(no_folder)) return files.error;
  return {};
}

// libarchive names a failed allocation in its decoder only by liblzma's code; either way it is memory.
std::string archive_failure(archive* reader, const char* fallback) {
  const char* text = archive_error_string(reader);
  if (archive_errno(reader) == ENOMEM || (text && std::strstr(text, "memory"))) return too_much_memory;
  return text && *text ? text : fallback;
}

std::string extract_libarchive(const Options& options, Writer& writer, Progress& progress, Report& report) {
  if (const auto refused = claim(options.memory + overhead, report.claim_ms); !refused.empty()) return refused;
  archive* reader = archive_read_new();
  if (!reader) return too_much_memory;
  archive_read_support_format_7zip(reader);
  std::vector<const char*> files;
  for (const auto& path : options.sources) files.push_back(path.c_str());
  files.push_back(nullptr);
  std::string error;
  if (archive_read_open_filenames(reader, files.data(), read_block) != ARCHIVE_OK) error = archive_failure(reader, "Unsupported or incomplete archive.");
  archive_entry* entry = nullptr;
  int result = ARCHIVE_OK;
  std::uint64_t declared = 0;
  // A warning is a header libarchive could only read in part, such as a name it could not convert.
  while (error.empty() && !stopping() && ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK || result == ARCHIVE_WARN)) {
    progress.beat();
    const char* raw = archive_entry_pathname_utf8(entry);
    if (!raw) {
      error = "Archive has a file name this console cannot read.";
      break;
    }
    const auto type = archive_entry_filetype(entry);
    const bool link = archive_entry_symlink(entry) || archive_entry_hardlink(entry);
    const auto size = archive_entry_size(entry);
    if (link || (type != AE_IFREG && type != AE_IFDIR)) {
      error = unsafe;
      break;
    }
    if (type == AE_IFREG && size < 0) {
      error = too_large;
      break;
    }
    int fd = -1;
    std::string name;
    error = create_entry(options, raw, type == AE_IFDIR, archive_entry_perm(entry) & 0111, std::uint64_t(std::max<la_int64_t>(size, 0)),
                         declared, fd, name);
    if (!error.empty() || type == AE_IFDIR) continue;
    writer.begin(fd, name);
    const void* data = nullptr;
    size_t bytes = 0;
    la_int64_t offset = 0;
    std::uint64_t file_written = 0;
    while (!stopping() && (result = archive_read_data_block(reader, &data, &bytes, &offset)) == ARCHIVE_OK) {
      // 7z entries come in order; a gap would be a sparse file, which 7-Zip does not write.
      if (offset < 0 || std::uint64_t(offset) != file_written) {
        error = damaged;
        break;
      }
      if (bytes > options.limit - progress.written) {
        error = too_large;
        break;
      }
      if (!writer.append(static_cast<const unsigned char*>(data), bytes)) break;
      file_written += bytes;
      progress.written += bytes;
      progress.beat();
    }
    if (error.empty() && !stopping() && result != ARCHIVE_EOF && result != ARCHIVE_OK) error = archive_failure(reader, damaged);
    if (error.empty()) error = writer.error();
    if (!error.empty() || stopping()) break;
    writer.end(std::uint64_t(size));
  }
  if (error.empty() && !stopping() && result != ARCHIVE_EOF) error = archive_failure(reader, "Incomplete archive headers.");
  archive_read_free(reader);
  return error;
}
}

int run() {
  // Directories and files get the engine's modes (worker_io.hpp), whatever the umask.
  umask(0);
  Options options;
  if (!read_options(options)) {
    say("fail Invalid 7z worker request.");
    return 1;
  }
  try {
    std::thread(control).detach();
  } catch (...) {
    say("fail 7z extraction failed.");
    return 1;
  }
  say("ready");
  CrcGenerateTable();
  // "k" every second says this process is alive while the claim, a long fsync, a slow block or the
  // download it waits for holds it.
  std::mutex alive_mutex;
  std::condition_variable alive_wake;
  bool done = false;
  std::thread alive;
  try {
    alive = std::thread([&] {
      std::unique_lock lock(alive_mutex);
      while (!alive_wake.wait_for(lock, std::chrono::seconds(1), [&] { return done; })) say("k");
    });
  } catch (...) {
  }
  std::string error;
  {
    Progress progress;
    std::unique_ptr<Writer> writer;
    Report report;
    Input input;
    const auto started = now_ns();
    try {
      if (input.open(options, error)) {
        writer = std::make_unique<Writer>();
        bool fallback = false;
        error = extract_sdk(options, input, *writer, progress, report, fallback);
        if (fallback && error.empty() && !stopping()) error = extract_libarchive(options, *writer, progress, report);
      }
    } catch (std::bad_alloc&) {
      error = too_much_memory;
    } catch (...) {
      error = "7z extraction failed.";
    }
    if (writer) {
      if (error.empty() && !stopping()) say("s");
      const auto written = writer->finish();
      if (error.empty()) error = written;
    }
    const auto ms = [](std::uint64_t ns) { return std::to_string(ns / 1000000) + "ms"; };
    std::string timings = "t threads=" + std::to_string(report.threads) + " cores=" + std::to_string(cores()) + " claim=" +
                          std::to_string(report.claim_ms) + "ms decoder=" + (report.sdk ? "sdk" : "libarchive") + " block=" +
                          std::to_string(report.block >> 20) + "MiB elapsed=" + ms(now_ns() - started);
    if (writer) {
      timings += " write=" + ms(writer->write_ns) + "/" + std::to_string(writer->writes) + " fsync=" + ms(writer->sync_ns) + "/" +
                 std::to_string(writer->syncs) + " waited=" + ms(writer->wait_ns);
    }
    if (input.growing()) timings += " download-wait=" + ms(input.waited_ns);
    if (const std::uint64_t cpu = process_cpu_us()) timings += " cpu=" + ms(cpu * 1000);
    say(timings);
  }
  {
    std::lock_guard lock(alive_mutex);
    done = true;
  }
  alive_wake.notify_all();
  if (alive.joinable()) alive.join();
  if (cancelled) say("cancelled");
  else say(error.empty() ? "ok" : "fail " + error);
  return error.empty() ? 0 : 1;
}
}
