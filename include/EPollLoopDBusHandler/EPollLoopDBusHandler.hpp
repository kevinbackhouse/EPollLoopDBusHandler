// Copyright 2021 Kevin Backhouse.
//
// This file is part of EPollLoopDBusHandler.
//
// EPollLoopDBusHandler is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// EPollLoopDBusHandler is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with EPollLoopDBusHandler.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <EPollLoop.hpp>
#include <dbus.hpp>
#include <utils.hpp>
#include <map>
#include <queue>

// epoll handler which parses incoming DBus messages.
class DBusHandler : public EPollHandlerInterface {
public:
  typedef std::function<int(const DBusMessage& message, bool isError)> reply_cb_t;
  typedef uint32_t serialNumber_t;

private:
  class SendBuf final {
    char* buf_;
    size_t bufsize_;
    size_t pos_;

  public:
    SendBuf(size_t bufsize);

    SendBuf(SendBuf&&);

    // Calls `free(buf_)`.
    ~SendBuf();

    // Number of bytes still left to send.
    size_t remaining() const noexcept { return bufsize_ - pos_; }

    // Get a pointer to the current position in the buffer.
    char* curr() const noexcept { return buf_ + pos_; }

    // Update `pos_` after receiving data.
    void incr(const ssize_t sendsize) noexcept {
      pos_ += sendsize;
    }
  };

  serialNumber_t serialNumber_ = 0x1000;

  // `message_` is null until a complete message is received.
  std::unique_ptr<DBusMessage> message_;
  Parse parse_;

  // Because the return type of `Parse::minRequiredBytes()` is `uint8_t`,
  // the maximum number of bytes that we might need to save is 0xFE.
  char saved_[0xFE];
  uint8_t savedsize_ = 0;

  // This is the number of padding bytes that we need to insert before
  // the bytes in saved_ to preserve correct alignment. This number is
  // usually zero, and is always less than 8.
  uint8_t savedalign_ = 0;

  // Queue of messages that are waiting to be sent.
  std::queue<SendBuf> sendqueue_;

  // Callback functions are added to this map by send_call().
  // They are indexed by the serialNumber that was used in the call.
  // When the reply or error reply comes in, the callback is invoked
  // and then removed from the map.
  std::map<serialNumber_t, reply_cb_t> reply_callbacks_;

public:
  explicit DBusHandler(AutoCloseFD&& fd) :
    EPollHandlerInterface(fd.release()),
    parse_(DBusMessage::parseLE(message_))
  {}

  explicit DBusHandler(const char* socketpath) :
    EPollHandlerInterface(open_dbus_socket(socketpath)),
    parse_(DBusMessage::parseLE(message_))
  {}

  virtual ~DBusHandler();

  // Open a UNIX domain socket to connect to message bus.
  static int open_dbus_socket(const char* socketpath);

private:
  int init() noexcept override final;

  int process_read() noexcept override final;

  int process_write() noexcept override final;

  // Wrapper around parse_message() that catches any exceptions.
  int parse_message_noexcept(char* buf, size_t pos, size_t remaining) noexcept;

  // Feeds the buffer into the message parser.
  int parse_message(char* buf, size_t pos, size_t remaining);

  // This is a helper method for process_read(). It is called when a
  // complete message has been received. (The message is stored in the
  // message_ field.)
  int process_message();

  // This method is called by process_message() when we receive a reply
  // or error reply. It checks the reply serial number and invokes the
  // corresponding callback function from the calbacks_ map.
  int dispatch_reply(bool isError);

  // Send the buffer. (If there is already a backlog of messages to send,
  // then it is added to sendqueue_.)
  void sendbuf(SendBuf&& buf);

  // Serialize the message and send it.
  void send_message(const DBusMessage& message);

protected:
  // This is called when we receive an incoming METHOD_CALL message.
  virtual void receive_call(const DBusMessage& call) = 0;

  // This is called when we receive an incoming signal.
  virtual void receive_signal(const DBusMessage& sig) = 0;

  // This is called when we receive an incoming error that isn't a response
  // one of our method calls.
  virtual void receive_error(const DBusMessage& err) = 0;

  // This is called after we have finished connecting to the dbus-daemon.
  virtual void accept() = 0;

  // This is called when the socket disconnects, for notification purposes.
  // If you aren't interested in this notification then just override it
  // with a no-op function.
  virtual void disconnect() noexcept = 0;

public:
  // This is used to emit error messages. It's a virtual function so
  // that you can implement it in the most appropriate way.
  virtual void logerror(const char*) noexcept = 0;

  void send_call(
    std::unique_ptr<DBusMessageBody>&& body,
    std::string&& path,
    std::string&& interface,
    std::string&& destination,
    std::string&& member,
    reply_cb_t cb,
    const MessageFlags flags = MSGFLAGS_EMPTY
  );

  void send_reply(
    const uint32_t replySerialNumber, // serial number that we are replying to
    std::unique_ptr<DBusMessageBody>&& body,
    std::string&& destination
  );

  void send_error_reply(
    const uint32_t replySerialNumber, // serial number that we are replying to
    std::string&& destination,
    std::string&& errmsg
  );

  typedef std::function<int(const std::string&)> hello_cb_t;

  // The hello message is always the first message that you have to send.
  // This is just a simple wrapper around send_call().
  void send_hello(hello_cb_t cb);

  // Only intended to be used by DBusAuthHandler.
  int getSock() const {return sock_; }
};

// Before we can start sending D-Bus messages, we have to do the auth
// sequence. This class handles the auth sequence, then replaces itself
// with a DBusHandler (by overriding the `getNextHandler()` method).
class DBusAuthHandler : public EPollHandlerInterface {
  // The UID to send in the initial AUTH message.
  const uid_t auth_uid_;

  // This is the DBusHander that will used once the auth step is complete.
  // The pointer is stored in `handler_` until auth is successful, then it
  // is transferred into `nexthandler_`, so that `getNextHandler()` will
  // only return the handler if auth is successful.
  DBusHandler* handler_;
  DBusHandler* nexthandler_ = nullptr;

  // The total auth sequence only takes a few hundred bytes in each
  // direction, so it's sufficient to use two fixed-size buffers.
  char sendbuf_[256];
  size_t sendpos_ = 0;
  size_t sendremaining_ = 0;
  char recvbuf_[256];
  size_t recvpos_ = 0;

public:
  DBusAuthHandler(
    uid_t auth_uid, DBusHandler* handler
  ) :
    EPollHandlerInterface(handler->getSock()),
    auth_uid_(auth_uid),
    handler_(handler)
  {}

  ~DBusAuthHandler() {
    delete handler_;
    delete nexthandler_;
  }

private:
  int init() noexcept override final;

  int process_read() noexcept override final;

  int process_write() noexcept override final;

  EPollHandlerInterface* getNextHandler() noexcept override final;
};
