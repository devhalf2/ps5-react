// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The daemon's HTTP/1.1 server on a loopback port: keep-alive, Content-Length bodies, at most
// kMaxConnections clients. Single-threaded: run() polls the sockets and calls the handler on the
// caller's thread, which is the JavaScript thread, so requests reach JS without a lock.
namespace http {
inline constexpr std::size_t kMaxBody = 1024 * 1024;
inline constexpr std::size_t kMaxHead = 16 * 1024;
inline constexpr std::size_t kMaxConnections = 8;

using Fields = std::vector<std::pair<std::string, std::string>>;

struct Request {
  std::string method, path, query; // path and query as sent, split at the first '?'
  Fields headers;                  // names in lowercase, repeats joined with ", "
  std::string body;
  const std::string* header(const char* name) const;
};

struct Response {
  int status = 200;
  Fields headers; // framing (Content-Length, Connection, Transfer-Encoding) is the server's
  std::string body;
};

class Server {
public:
  // Answers a request at once with that status instead of handing it on, or 0 to hand it on.
  using Check = std::function<int(const Request&)>;
  // Receives each request that passed the check; answer it with respond(ticket, ...), now or later.
  using Handler = std::function<void(std::uint64_t ticket, Request&& request)>;

  Server();
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds 127.0.0.1:port. 0, or the errno that stopped it (EADDRINUSE when the port is taken).
  // Clients wait in the backlog until serve().
  int listen(int port);
  void serve(Check check, Handler handler);
  bool serving() const;
  // Waits up to timeout_ms for the sockets, then accepts, reads, hands requests on and writes.
  void run(int timeout_ms);
  // Answers the request with this ticket; ignored when its client has gone or it was answered.
  void respond(std::uint64_t ticket, Response response);
  // Writes what is answered for up to timeout_ms, still bound to the port (new clients wait in the backlog).
  void flush(int timeout_ms);
  // Stops accepting and writes what is answered for up to timeout_ms, then closes every socket.
  void close(int timeout_ms);

private:
  struct Connection;
  void accept_clients();
  void read(Connection& connection);
  void parse(Connection& connection);
  void write(Connection& connection);
  void fail(Connection& connection, int status, const char* message);
  void queue(Connection& connection, const Response& response, bool head);

  int listener_ = -1;
  Check check_;
  Handler handler_;
  std::uint64_t next_ticket_ = 1;
  std::vector<std::unique_ptr<Connection>> connections_;
};
} // namespace http
