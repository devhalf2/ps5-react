// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "http_server.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace http {
namespace {
// A client that sends nothing for this long is dropped: an idle keep-alive, or one that stalls mid-request.
constexpr std::int64_t kIdleMs = 60'000;
// After the last answer on a closing connection, what the client still sends is read and dropped for
// this long: closing with unread input resets the connection, and the client may lose the answer (a 413
// sent while the body still arrives).
constexpr std::int64_t kLingerMs = 2'000;

std::int64_t now_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1'000'000;
}

// Non-blocking, and closed in the workers the daemon starts (rar-extract), which would otherwise hold
// the port after it exits.
bool nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

const char* reason(int status) {
  switch (status) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content";
    case 206: return "Partial Content"; case 301: return "Moved Permanently"; case 302: return "Found";
    case 304: return "Not Modified"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 409: return "Conflict"; case 411: return "Length Required"; case 413: return "Content Too Large";
    case 416: return "Range Not Satisfiable"; case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large"; case 500: return "Internal Server Error";
    case 501: return "Not Implemented"; case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported"; default: return "Status";
  }
}

bool token_character(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::strchr("!#$%&'*+-.^_`|~", c);
}

bool token(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c && token_character(c); });
}

std::string lowercase(std::string text) {
  for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}

std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

// Whether a comma-separated header value lists `word`, case-insensitively.
bool lists(const std::string* value, const char* word) {
  if (!value) return false;
  const std::string text = lowercase(*value);
  for (std::size_t start = 0; start <= text.size();) {
    const auto comma = text.find(',', start);
    if (trim(text.substr(start, comma - start)) == word) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

std::string error_body(const char* message) {
  std::string body = "{\"error\":{\"message\":\"";
  for (const char* c = message; *c; ++c) {
    if (*c == '"' || *c == '\\') body += '\\';
    body += *c;
  }
  return body + "\"}}";
}
} // namespace

const std::string* Request::header(const char* name) const {
  for (const auto& [key, value] : headers) if (key == name) return &value;
  return nullptr;
}

struct Server::Connection {
  int fd = -1;
  std::string input, output;
  std::size_t sent = 0;
  std::int64_t active = 0;
  std::uint64_t ticket = 0; // the request JS holds, 0 when none
  bool keep_alive = true, closing = false, closed = false, continued = false, head = false, lingering = false;
};

Server::Server() = default;
Server::~Server() { close(0); }

bool Server::serving() const { return listener_ >= 0 && handler_; }

int Server::listen(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return errno;
  const int on = 1;
  // Lets a restarted daemon bind past the old one's TIME_WAIT; BSD still refuses a second listener.
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(fd, 16) != 0 || !nonblocking(fd)) {
    const int error = errno;
    ::close(fd);
    return error ? error : EIO;
  }
  listener_ = fd;
  return 0;
}

void Server::serve(Check check, Handler handler) {
  check_ = std::move(check);
  handler_ = std::move(handler);
}

void Server::run(int timeout_ms) {
  std::vector<pollfd> fds;
  const bool accepting = serving();
  if (accepting) fds.push_back({listener_, POLLIN, 0});
  for (const auto& connection : connections_) {
    // While JS holds a request the client is still read, so its hang-up drops the connection.
    const bool writing = connection->sent < connection->output.size();
    fds.push_back({connection->fd, static_cast<short>(writing ? POLLIN | POLLOUT : POLLIN), 0});
  }
  if (fds.empty()) {
    if (timeout_ms > 0) usleep(static_cast<useconds_t>(timeout_ms) * 1000);
    return;
  }
  if (poll(fds.data(), fds.size(), timeout_ms) < 0 && errno != EINTR) return;
  std::size_t index = 0;
  if (accepting && (fds[index++].revents & POLLIN)) accept_clients();
  // Handlers may answer other connections and the list may grow meanwhile; closed ones go at the end.
  const std::size_t polled = fds.size() - index;
  for (std::size_t i = 0; i < polled && i < connections_.size(); ++i) {
    Connection& connection = *connections_[i];
    const short events = fds[index + i].revents;
    if (connection.closed) continue;
    if (events & (POLLIN | POLLHUP | POLLERR)) read(connection);
    if (!connection.closed && (events & POLLOUT)) write(connection);
  }
  const std::int64_t now = now_ms();
  for (auto& connection : connections_)
    if (!connection->closed && !connection->ticket && connection->sent >= connection->output.size() &&
        now - connection->active > (connection->lingering ? kLingerMs : kIdleMs)) {
      ::close(connection->fd);
      connection->closed = true;
    }
  std::erase_if(connections_, [](const auto& connection) { return connection->closed; });
}

void Server::accept_clients() {
  for (;;) {
    const int fd = accept(listener_, nullptr, nullptr);
    if (fd < 0) return;
    if (connections_.size() >= kMaxConnections) {
      // Best effort: a full server says so rather than leaving the client to time out.
      const std::string body = error_body("the daemon serves at most 8 connections at once");
      const std::string answer = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      (void)!::send(fd, answer.data(), answer.size(), 0);
      ::close(fd);
      continue;
    }
    if (!nonblocking(fd)) { ::close(fd); continue; }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    auto connection = std::make_unique<Connection>();
    connection->fd = fd;
    connection->active = now_ms();
    connections_.push_back(std::move(connection));
  }
}

void Server::read(Connection& connection) {
  char buffer[16384];
  for (;;) {
    const ssize_t count = recv(connection.fd, buffer, sizeof buffer, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count <= 0) {
      ::close(connection.fd);
      connection.closed = true;
      return;
    }
    if (connection.lingering) continue;
    connection.active = now_ms();
    connection.input.append(buffer, static_cast<std::size_t>(count));
    // Pipelined requests wait their turn; past one request's worth the client is not following HTTP.
    if (connection.input.size() > kMaxHead + kMaxBody) {
      if (connection.ticket) {
        ::close(connection.fd);
        connection.closed = true;
        return;
      }
      break;
    }
  }
  parse(connection);
}

void Server::parse(Connection& connection) {
  // Nothing new reaches JS once close() began (listener_ is gone by then).
  if (listener_ < 0 || connection.ticket || connection.closing || connection.closed || connection.lingering ||
      connection.sent < connection.output.size()) return;
  const auto end = connection.input.find("\r\n\r\n");
  if (end == std::string::npos) {
    if (connection.input.size() > kMaxHead) fail(connection, 431, "the request head is larger than 16 KiB");
    return;
  }
  if (end > kMaxHead) return fail(connection, 431, "the request head is larger than 16 KiB");
  Request request;
  const std::string head = connection.input.substr(0, end);
  auto line_end = head.find("\r\n");
  const std::string line = head.substr(0, line_end);
  const auto space = line.find(' '), last = line.rfind(' ');
  if (space == std::string::npos || last == space) return fail(connection, 400, "malformed request line");
  request.method = line.substr(0, space);
  const std::string target = line.substr(space + 1, last - space - 1), version = line.substr(last + 1);
  if (!token(request.method) || target.empty() || target[0] != '/' || target.find(' ') != std::string::npos)
    return fail(connection, 400, "malformed request line");
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return fail(connection, 505, "only HTTP/1.1 and HTTP/1.0");
  const auto question = target.find('?');
  request.path = target.substr(0, question);
  request.query = question == std::string::npos ? "" : target.substr(question + 1);
  while (line_end != std::string::npos) {
    const auto start = line_end + 2;
    line_end = head.find("\r\n", start);
    const std::string field = head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
    const auto colon = field.find(':');
    if (colon == std::string::npos || !token(field.substr(0, colon))) return fail(connection, 400, "malformed header");
    const std::string name = lowercase(field.substr(0, colon)), value = trim(field.substr(colon + 1));
    if (value.find_first_of(std::string("\r\n\0", 3)) != std::string::npos) return fail(connection, 400, "malformed header");
    auto existing = std::find_if(request.headers.begin(), request.headers.end(), [&](const auto& item) { return item.first == name; });
    if (existing == request.headers.end()) request.headers.emplace_back(name, value);
    else existing->second += ", " + value;
  }
  if (request.header("transfer-encoding")) return fail(connection, 501, "send the body with Content-Length");
  std::size_t length = 0;
  if (const std::string* value = request.header("content-length")) {
    if (value->empty() || value->size() > 10 || value->find_first_not_of("0123456789") != std::string::npos)
      return fail(connection, 400, "malformed Content-Length");
    length = static_cast<std::size_t>(std::strtoull(value->c_str(), nullptr, 10));
    if (length > kMaxBody) return fail(connection, 413, "the body is larger than 1 MiB");
  }
  connection.keep_alive = version == "HTTP/1.1" ? !lists(request.header("connection"), "close")
                                                : lists(request.header("connection"), "keep-alive");
  if (connection.input.size() < end + 4 + length) {
    if (!connection.continued && lists(request.header("expect"), "100-continue")) {
      connection.continued = true;
      connection.output += "HTTP/1.1 100 Continue\r\n\r\n";
      write(connection);
    }
    return;
  }
  request.body = connection.input.substr(end + 4, length);
  connection.input.erase(0, end + 4 + length);
  connection.continued = false;
  if (const int status = check_ ? check_(request) : 0) {
    Response response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = error_body(status == 401 ? "missing or wrong x-pandora-token" : reason(status));
    queue(connection, response, request.method == "HEAD");
    write(connection);
    return;
  }
  connection.ticket = next_ticket_++;
  connection.closing = !connection.keep_alive;
  connection.head = request.method == "HEAD";
  handler_(connection.ticket, std::move(request));
}

void Server::respond(std::uint64_t ticket, Response response) {
  for (auto& item : connections_) {
    Connection& connection = *item;
    if (connection.closed || connection.ticket != ticket || !ticket) continue;
    connection.ticket = 0;
    queue(connection, response, connection.head);
    write(connection);
    return;
  }
}

void Server::queue(Connection& connection, const Response& response, bool head) {
  const bool body_allowed = response.status != 204 && response.status != 304;
  std::string text = "HTTP/1.1 " + std::to_string(response.status) + " " + reason(response.status) + "\r\n";
  for (const auto& [name, value] : response.headers) {
    const std::string key = lowercase(name);
    if (key == "content-length" || key == "connection" || key == "transfer-encoding") continue;
    text += name + ": " + value + "\r\n";
  }
  if (response.status != 204) text += "Content-Length: " + std::to_string(body_allowed ? response.body.size() : 0) + "\r\n";
  text += connection.keep_alive && !connection.closing ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
  if (!head && body_allowed) text += response.body;
  connection.output += text;
  if (!connection.keep_alive) connection.closing = true;
}

void Server::fail(Connection& connection, int status, const char* message) {
  Response response;
  response.status = status;
  response.headers.emplace_back("Content-Type", "application/json");
  response.body = error_body(message);
  connection.keep_alive = false;
  connection.input.clear();
  queue(connection, response, false);
  write(connection);
}

void Server::write(Connection& connection) {
  while (connection.sent < connection.output.size()) {
    const ssize_t count = send(connection.fd, connection.output.data() + connection.sent,
                               connection.output.size() - connection.sent, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (count <= 0) {
      ::close(connection.fd);
      connection.closed = true;
      return;
    }
    connection.sent += static_cast<std::size_t>(count);
    connection.active = now_ms();
  }
  connection.output.clear();
  connection.sent = 0;
  if (connection.closing && !connection.ticket) {
    shutdown(connection.fd, SHUT_WR);
    connection.lingering = true;
    connection.input.clear();
    connection.active = now_ms();
    return;
  }
  parse(connection); // A pipelined request may be waiting.
}

void Server::close(int timeout_ms) {
  if (listener_ >= 0) ::close(listener_);
  listener_ = -1;
  for (const std::int64_t deadline = now_ms() + timeout_ms; now_ms() < deadline;) {
    std::vector<pollfd> fds;
    for (const auto& connection : connections_)
      if (!connection->closed && connection->sent < connection->output.size()) fds.push_back({connection->fd, POLLOUT, 0});
    if (fds.empty()) break;
    poll(fds.data(), fds.size(), 50);
    for (auto& connection : connections_)
      if (!connection->closed && connection->sent < connection->output.size()) write(*connection);
  }
  for (auto& connection : connections_)
    if (!connection->closed) ::close(connection->fd);
  connections_.clear();
}
} // namespace http
