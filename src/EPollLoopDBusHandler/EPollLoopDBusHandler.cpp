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

#include <sys/un.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <dbus_utils.hpp>
#include <dbus_serialize.hpp>
#include "EPollLoopDBusHandler.hpp"

// D-Bus messages are expected to be 8-byte aligned.
const size_t dbus_alignment = sizeof(uint64_t);

DBusHandler::SendBuf::SendBuf(size_t bufsize) :
  buf_(static_cast<char*>(malloc(bufsize))),
  bufsize_(bufsize),
  pos_(0)
{
  if (!buf_) {
    throw std::bad_alloc();
  }
}

DBusHandler::SendBuf::SendBuf(SendBuf&& that) :
  buf_(that.buf_),
  bufsize_(that.bufsize_),
  pos_(that.pos_)
{
  that.buf_ = 0;
  that.bufsize_ = 0;
  that.pos_ = 0;
}

DBusHandler::SendBuf::~SendBuf() {
  free(buf_);
}

int DBusHandler::open_dbus_socket(const char* socketpath) {
  sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketpath);

  AutoCloseFD fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0));
  if (fd.get() < 0) {
    throw ErrorWithErrno(_s("Could not open socket: ") + _s(socketpath));
  }

  if (connect(fd.get(), (sockaddr*)(&address), sizeof(address)) < 0) {
    throw ErrorWithErrno(_s("Could not connect to socket: ") + _s(socketpath));
  }

  return fd.release();
}

int DBusHandler::init() noexcept {
  try {
    accept();
    return 0;
  } catch (ErrorWithErrno& e) {
    const int err = e.getErrno();
    char errmsg[128];
    snprintf(
      errmsg, sizeof(errmsg),
      "Exception caught in DBusHandler::init(): %s (%s)",
      e.what(), strerror(err)
    );
    logerror(errmsg);
  } catch (std::exception& e) {
    char errmsg[128];
    snprintf(
      errmsg, sizeof(errmsg),
      "Exception caught in DBusHandler::init(): %s",
      e.what()
    );
    logerror(errmsg);
  } catch(...) {
    logerror("Unknown exception caught in DBusHandler::process_message()");
  }

  disconnect();
  return -1;
}

int DBusHandler::process_read() noexcept {
  while (true) {
    char buf[0x1000];

    // Make sure that the `memcpy` below cannot overflow.
    static_assert(sizeof(buf) >= sizeof(saved_) + dbus_alignment);

    const size_t recvoffset = savedalign_ + savedsize_;
    const ssize_t recvsize =
      recv(sock_, buf + recvoffset, sizeof(buf) - recvoffset, 0);

    if (recvsize == 0) {
      // Peer has closed the connection.
      disconnect();
      return -1;
    }

    if (recvsize < 0) {
      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // Need to wait for more input. (We will get a notification from
        // epoll when that happens.)
        return 0;
      }

      char errmsg[128];
      snprintf(
        errmsg, sizeof(errmsg),
        "Error in DBusHandler::process_read(): %s",
        strerror(err)
      );
      logerror(errmsg);
      disconnect();
      return -1;
    }

    // Restore the unprocessed data from the last read.
    memcpy(buf + savedalign_, saved_, savedsize_);

    size_t remaining = recvsize + savedsize_;
    if (parse_message_noexcept(buf, savedalign_, remaining) < 0) {
      disconnect();
      return -1;
    }
  }
}

int DBusHandler::process_write() noexcept {
  while (!sendqueue_.empty()) {
    SendBuf& buf = sendqueue_.front();
    while (buf.remaining() > 0) {
      const ssize_t wr = send(
        sock_, buf.curr(), buf.remaining(), MSG_NOSIGNAL
      );

      if (wr < 0)  {
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
          // The socket buffer is full, so we need to wait for epoll
          // to tell us that we can resume sending.
          return 0;
        }

        char errmsg[128];
        snprintf(
          errmsg, sizeof(errmsg),
          "Error in DBusHandler::process_write(): %s",
          strerror(err)
        );
        logerror(errmsg);

        disconnect();
        return -1;
      }

      buf.incr(wr);
    }
    sendqueue_.pop();
  }
  return 0;
}

int DBusHandler::parse_message_noexcept(
  char* buf, size_t pos, size_t remaining
) noexcept {
  try {
    return parse_message(buf, pos, remaining);
  } catch (ErrorWithErrno& e) {
    const int err = e.getErrno();
    char errmsg[128];
    snprintf(
      errmsg, sizeof(errmsg),
      "Exception caught in DBusHandler::process_read(): %s (%s)",
      e.what(), strerror(err)
    );
    logerror(errmsg);
    return -1;
  } catch (std::exception& e) {
    char errmsg[128];
    snprintf(
      errmsg, sizeof(errmsg),
      "Exception caught in DBusHandler::process_read(): %s",
      e.what()
    );
    logerror(errmsg);
    return -1;
  } catch(...) {
    logerror("Unknown exception caught in DBusHandler::process_read()");
    return -1;
  }
}

int DBusHandler::parse_message(char* buf, size_t pos, size_t remaining) {
  while (true) {
    const size_t required = parse_.maxRequiredBytes();
    if (required == 0) {
      // Parsing is complete.
      if (process_message() < 0) {
        return -1;
      }
      message_.reset();
      // TODO: add support for big endian.
      parse_.reset(DBusMessage::parseLE(message_));
      // Make sure that the memory is aligned for the next message.
      if (pos % dbus_alignment != 0) {
        memmove(buf, buf+pos, remaining);
        pos = 0;
      }
      // Parse the next message.
      continue;
    }

    if (remaining >= required) {
      parse_.parse(buf + pos, required);
      pos += required;
      remaining -= required;
      continue;
    }

    break;
  }

  if (remaining < parse_.minRequiredBytes()) {
    // Not enough bytes to continue parsing, so we save the remaining
    // bytes in `saved_`. The return type of `minRequiredBytes` is
    // `uint8_t`, so the number of remaining bytes must be less than
    // 0xFF.
    assert(remaining < 0xFF);
    memcpy(saved_, buf + pos, remaining);
  } else {
    // The parser can consume all the remaining bytes.
    parse_.parse(buf + pos, remaining);
    pos += remaining;
    remaining = 0;
  }
  savedsize_ = remaining;
  savedalign_ = pos % dbus_alignment;

  return 0;
}

// Process an incoming message. Different things happen, depending on
// whether it's a call, reply, error, or signal.
int DBusHandler::process_message() {
  switch (message_->getHeader_messageType()) {
  case MSGTYPE_METHOD_CALL:
    receive_call(*message_);
    return 0;

  case MSGTYPE_METHOD_RETURN:
    return dispatch_reply(false);

  case MSGTYPE_ERROR:
    return dispatch_reply(true);

  case MSGTYPE_SIGNAL:
    receive_signal(*message_);
    return 0;

  default:
    logerror("Unknown message type in DBusHandler::process_message()");
    return -1;
  }
}

int DBusHandler::dispatch_reply(bool isError) {
  const serialNumber_t replySerial =
    message_->getHeader_lookupField(MSGHDR_REPLY_SERIAL).getValue()->toUint32().getValue();;
  auto i = reply_callbacks_.find(replySerial);
  if (i != reply_callbacks_.end()) {
    // Call the callback function.
    const int r = i->second(*message_, isError);
    reply_callbacks_.erase(i);
    if (r < 0) {
      return -1;
    } else {
      return 0;
    }
  } else if (message_->getHeader_messageType() == MSGTYPE_ERROR) {
    receive_error(*message_);
    return 0;
  } else {
    char errmsg[128];
    snprintf(
      errmsg, sizeof(errmsg),
      "Unknown reply serial number in DBusHandler::dispatch_reply(): %u",
      replySerial
    );
    logerror(errmsg);
    return -1;
  }
}

void DBusHandler::sendbuf(SendBuf&& buf) {
  if (!sendqueue_.empty()) {
    // `sendqueue_` isn't empty, which means two things:
    //
    // 1. The remaining bytes in `sendqueue_` need to be sent before we can
    //    send the contents of `buf`.
    // 2. A previous send was incomplete, so we are already waiting for
    //    epoll to notify us that we can resume sending.
    //
    // Therefore, we cannot call `send` right now. We can only append the
    // contents of `buf` to `sendqueue_` and wait for the notification from
    // epoll.
    sendqueue_.push(std::move(buf));
  } else {
    // First try to send the message directly. If that's unsuccessful,
    // or we only manage to send part of the message, then we'll add
    // the rest of the message to `sendqueue_` and wait for an EPOLLOUT
    // notification.
    const ssize_t wr = send(sock_, buf.curr(), buf.remaining(), MSG_NOSIGNAL);
    if (wr < 0) {
      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // Copy everything to sendbuf_.
        sendqueue_.push(std::move(buf));
      } else {
        throw ErrorWithErrno("Send failed");
      }
    } else {
      // Check if the send was incomplete.
      buf.incr(wr);
      if (buf.remaining() > 0) {
        sendqueue_.push(std::move(buf));
      }
    }
  }
}

void DBusHandler::send_message(const DBusMessage& message) {
  std::vector<uint32_t> arraySizes;
  SerializerInitArraySizes s0(arraySizes);
  message.serialize(s0);

  const size_t size = s0.getPos();
  SendBuf buf(size);
  SerializeToBuffer<LittleEndian> s1(arraySizes, buf.curr());
  message.serialize(s1);

  sendbuf(std::move(buf));
}

void DBusHandler::send_call(
  std::unique_ptr<DBusMessageBody>&& body,
  std::string&& path,
  std::string&& interface,
  std::string&& destination,
  std::string&& member,
  reply_cb_t cb,
  const MessageFlags flags
) {
  const uint32_t serialNumber = serialNumber_++;

  std::unique_ptr<DBusMessage> message(
    mk_dbus_method_call_msg(
      serialNumber,
      std::move(body),
      std::move(path),
      std::move(interface),
      std::move(destination),
      std::move(member),
      0,
      flags
    )
  );

  send_message(*message);

  reply_callbacks_.insert({serialNumber, cb});
}

void DBusHandler::send_reply(
  const uint32_t replySerialNumber, // serial number that we are replying to
  std::unique_ptr<DBusMessageBody>&& body,
  std::string&& destination
) {
  const uint32_t serialNumber = serialNumber_++;

  std::unique_ptr<DBusMessage> message(
    mk_dbus_method_reply_msg(
      serialNumber,
      replySerialNumber,
      std::move(body),
      std::move(destination)
    )
  );

  send_message(*message);
}

void DBusHandler::send_error_reply(
  const uint32_t replySerialNumber, // serial number that we are replying to
  std::string&& destination,
  std::string&& errmsg
) {
  const uint32_t serialNumber = serialNumber_++;

  std::unique_ptr<DBusMessage> message(
    mk_dbus_method_error_reply_msg(
      serialNumber,
      replySerialNumber,
      std::move(destination),
      std::move(errmsg)
    )
  );

  send_message(*message);
}

void DBusHandler::send_hello(hello_cb_t cb) {
  send_call(
    DBusMessageBody::mk0(),
    _s("/org/freedesktop/DBus"),
    _s("org.freedesktop.DBus"),
    _s("org.freedesktop.DBus"),
    _s("Hello"),
    [this, cb](const DBusMessage& message, bool isError) -> int {
      if (isError) {
        throw Error("Received error reply to hello message.");
      }
      const std::string& busname =
        message.getBody().getElement(0)->toString().getValue();
      return cb(busname);
    }
  );
}

static size_t write_string(
  char* buf, size_t pos, size_t bufsize, const char* str
) {
  const size_t len = strlen(str);
  if (len > bufsize - pos) {
    throw std::out_of_range("Error in write_string()");
  }
  memcpy(&buf[pos], str, len);
  return pos + len;
}

int DBusAuthHandler::init() noexcept {
  size_t pos = 0;

  // The auth flow is really supposed to involve a few messages
  // going back and forth, but the sequence is always the same,
  // so it's easier just to send everything in one go.
  sendbuf_[pos++] = '\0';
  pos = write_string(sendbuf_, pos, sizeof(sendbuf_), "AUTH EXTERNAL ");

  // The UID needs to be converted to decimal, and then to hex.
  // For example: 1001 -> "1001" -> "31303031".
  char uidstr[16];
  snprintf(uidstr, sizeof(uidstr), "%d", auth_uid_);
  const size_t n = strlen(uidstr);
  for (size_t i = 0; i < n; i++) {
    char tmp[4];
    snprintf(tmp, sizeof(tmp), "%.2x", (int)uidstr[i]);
    pos = write_string(sendbuf_, pos, sizeof(sendbuf_), tmp);
  }
  pos = write_string(sendbuf_, pos, sizeof(sendbuf_), "\r\n");
  pos = write_string(sendbuf_, pos, sizeof(sendbuf_), "NEGOTIATE_UNIX_FD\r\n");
  pos = write_string(sendbuf_, pos, sizeof(sendbuf_), "BEGIN\r\n");

  sendpos_ = 0;
  sendremaining_ = pos;

  return process_write();
}

int DBusAuthHandler::process_write() noexcept {
  while (sendremaining_ > 0) {
    const ssize_t wr = send(
      sock_, sendbuf_ + sendpos_, sendremaining_, MSG_NOSIGNAL
    );

    if (wr < 0)  {
      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // The socket buffer is full, so we need to wait for epoll
        // to tell us that we can resume sending.
        return 0;
      }

      if (handler_) {
        char errmsg[128];
        snprintf(
          errmsg, sizeof(errmsg),
          "Error in DBusAuthHandler::process_write(): %s",
          strerror(err)
        );
        handler_->logerror(errmsg);
      }

      return -1;
    }

    sendpos_ += wr;
    sendremaining_ -= wr;
  }
  return 0;
}

int DBusAuthHandler::process_read() noexcept {
  while (true) {
    const ssize_t recvsize =
      recv(sock_, recvbuf_ + recvpos_, sizeof(recvbuf_) - 1 - recvpos_, 0);

    if (recvsize == 0) {
      // Peer has closed the connection.
      return -1;
    }

    if (recvsize < 0) {
      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // Need to wait for more input. (We will get a notification from
        // epoll when that happens.)
        return 0;
      }

      if (handler_) {
        char errmsg[128];
        snprintf(
          errmsg, sizeof(errmsg),
          "Error in DBusAuthHandler::process_read(): %s",
          strerror(err)
        );
        handler_->logerror(errmsg);
      }

      return -1;
    }

    recvpos_ += recvsize;

    // We left a spare byte at the end of the buffer so that there's space
    // to nul-terminate the string.
    recvbuf_[recvpos_] = '\0';
    if (sscanf(recvbuf_, "OK %*s\r\nAGREE_UNIX_FD\r\n") == 0) {
      // AUTH was successful, so we can hand over to the DBusHandler. Move
      // the handler pointer into nexthandler_ so that it will be returned
      // by getNextHandler().
      std::swap(handler_, nexthandler_);

      // This handler will be closed and deleted, and nexthandler_ will be
      // installed to replace it.
      return -1;
    }
  }
}

EPollHandlerInterface* DBusAuthHandler::getNextHandler() noexcept {
  // Make sure that nexthandler_ is zeroed out so that we don't own the
  // pointer anymore.
  DBusHandler* h = nullptr;
  std::swap(h, nexthandler_);
  return h;
}
