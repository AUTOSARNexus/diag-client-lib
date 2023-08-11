/* Diagnostic Client library
 * Copyright (C) 2023  Avijit Dey
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "socket/tcp/tcp_client.h"

#include "common/logger.h"

namespace boost_support {
namespace socket {
namespace tcp {
// ctor
CreateTcpClientSocket::CreateTcpClientSocket(std::string_view local_ip_address, std::uint16_t local_port_num,
                                             TcpHandlerRead &&tcp_handler_read)
    : local_ip_address_{local_ip_address},
      local_port_num_{local_port_num},
      exit_request_{false},
      running_{false},
      tcp_handler_read_{std::move(tcp_handler_read)} {
  // Create socket
  tcp_socket_ = std::make_unique<TcpSocket>(io_context_);
  // Start thread to receive messages
  thread_ = std::thread([&]() {
    std::unique_lock<std::mutex> lck(mutex_);
    while (!exit_request_) {
      if (!running_) {
        cond_var_.wait(lck, [this]() { return exit_request_ || running_; });
      }
      if (!exit_request_.load()) {
        if (running_) {
          lck.unlock();
          HandleMessage();
          lck.lock();
        }
      }
    }
  });
}

// dtor
CreateTcpClientSocket::~CreateTcpClientSocket() {
  exit_request_ = true;
  running_ = false;
  cond_var_.notify_all();
  thread_.join();
}

bool CreateTcpClientSocket::Open() {
  TcpErrorCodeType ec{};
  bool ret_val{false};
  // Open the socket
  tcp_socket_->open(Tcp::v4(), ec);
  if (ec.value() == boost::system::errc::success) {
    // reuse address
    tcp_socket_->set_option(boost::asio::socket_base::reuse_address{true});
    // Set socket to non blocking
    tcp_socket_->non_blocking(false);
    //bind to local address and random port
    tcp_socket_->bind(Tcp::endpoint(TcpIpAddress::from_string(local_ip_address_), local_port_num_), ec);
    if (ec.value() == boost::system::errc::success) {
      // Socket binding success
      common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogDebug(
          __FILE__, __LINE__, __func__, [this](std::stringstream &msg) {
            Tcp::endpoint const endpoint_{tcp_socket_->local_endpoint()};
            msg << "Tcp Socket opened and bound to "
                << "<" << endpoint_.address().to_string() << "," << endpoint_.port() << ">";
          });
      ret_val = true;
    } else {
      // Socket binding failed
      common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogError(
          __FILE__, __LINE__, __func__,
          [ec](std::stringstream &msg) { msg << "Tcp Socket binding failed with message: " << ec.message(); });
      ret_val = false;
    }
  } else {
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogError(
        __FILE__, __LINE__, __func__,
        [ec](std::stringstream &msg) { msg << "Tcp Socket opening failed with error: " << ec.message(); });
  }
  return ret_val;
}

// connect to host
bool CreateTcpClientSocket::ConnectToHost(std::string_view host_ip_address, std::uint16_t host_port_num) {
  TcpErrorCodeType ec{};
  bool ret_val{false};
  // connect to provided ipAddress
  tcp_socket_->connect(Tcp::endpoint(TcpIpAddress::from_string(std::string{host_ip_address}), host_port_num), ec);
  if (ec.value() == boost::system::errc::success) {
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogDebug(
        __FILE__, __LINE__, __func__, [this](std::stringstream &msg) {
          Tcp::endpoint const endpoint_{tcp_socket_->remote_endpoint()};
          msg << "Tcp Socket connected to host "
              << "<" << endpoint_.address().to_string() << "," << endpoint_.port() << ">";
        });
    {  // start reading
      std::lock_guard<std::mutex> lock{mutex_};
      running_ = true;
      cond_var_.notify_all();
    }
    ret_val = true;
  } else {
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogError(
        __FILE__, __LINE__, __func__,
        [ec](std::stringstream &msg) { msg << "Tcp Socket connect to host failed with error: " << ec.message(); });
  }
  return ret_val;
}

// Disconnect from Host
bool CreateTcpClientSocket::DisconnectFromHost() {
  TcpErrorCodeType ec{};
  bool ret_val{false};

  // Graceful shutdown
  tcp_socket_->shutdown(TcpSocket::shutdown_both, ec);
  if (ec.value() == boost::system::errc::success) {
    // stop reading
    running_ = false;
    // Socket shutdown success
    ret_val = true;
  } else {
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogError(
        __FILE__, __LINE__, __func__, [ec](std::stringstream &msg) {
          msg << "Tcp Socket disconnection from host failed with error: " << ec.message();
        });
  }
  return ret_val;
}

// Function to transmit tcp messages
bool CreateTcpClientSocket::Transmit(TcpMessageConstPtr tcp_message) {
  TcpErrorCodeType ec{};
  bool ret_val{false};
  boost::asio::write(*tcp_socket_,
                     boost::asio::buffer(tcp_message->GetTxBuffer(), std::size_t(tcp_message->GetTxBuffer().size())),
                     ec);
  // Check for error
  if (ec.value() == boost::system::errc::success) {
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogDebug(
        __FILE__, __LINE__, __func__, [this](std::stringstream &msg) {
          Tcp::endpoint const endpoint_{tcp_socket_->remote_endpoint()};
          msg << "Tcp message sent to "
              << "<" << endpoint_.address().to_string() << "," << endpoint_.port() << ">";
        });
    ret_val = true;
  } else {
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogError(
        __FILE__, __LINE__, __func__,
        [ec](std::stringstream &msg) { msg << "Tcp message sending failed with error: " << ec.message(); });
  }
  return ret_val;
}

// Destroy the socket
bool CreateTcpClientSocket::Destroy() {
  // destroy the socket
  tcp_socket_->close();
  return true;
}

// Handle reading from socket
void CreateTcpClientSocket::HandleMessage() {
  TcpErrorCodeType ec{};
  // create and reserve the buffer
  TcpMessage::BufferType rx_buffer{};
  rx_buffer.resize(kDoipheadrSize);
  // start blocking read to read Header first
  boost::asio::read(*tcp_socket_, boost::asio::buffer(&rx_buffer[0u], kDoipheadrSize), ec);
  // Check for error
  if (ec.value() == boost::system::errc::success) {
    // read the next bytes to read
    std::uint32_t const read_next_bytes = [&rx_buffer]() noexcept -> std::uint32_t {
      return ((std::uint32_t)(
          (std::uint32_t)((rx_buffer[4u] << 24u) & 0xFF000000) | (std::uint32_t)((rx_buffer[5u] << 16u) & 0x00FF0000) |
          (std::uint32_t)((rx_buffer[6u] << 8u) & 0x0000FF00) | (std::uint32_t)((rx_buffer[7u] & 0x000000FF))));
    }();
    // reserve the buffer
    rx_buffer.resize(kDoipheadrSize + std::size_t(read_next_bytes));
    boost::asio::read(*tcp_socket_, boost::asio::buffer(&rx_buffer[kDoipheadrSize], read_next_bytes), ec);

    // all message received, transfer to upper layer
    Tcp::endpoint const endpoint_{tcp_socket_->remote_endpoint()};
    TcpMessagePtr tcp_rx_message{
        std::make_unique<TcpMessage>(endpoint_.address().to_string(), endpoint_.port(), std::move(rx_buffer))};
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogDebug(
        __FILE__, __LINE__, __func__, [endpoint_](std::stringstream &msg) {
          msg << "Tcp Message received from "
              << "<" << endpoint_.address().to_string() << "," << endpoint_.port() << ">";
        });
    // notify upper layer about received message
    tcp_handler_read_(std::move(tcp_rx_message));
  } else if (ec.value() == boost::asio::error::eof) {
    running_ = false;
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogDebug(
        __FILE__, __LINE__, __func__,
        [ec](std::stringstream &msg) { msg << "Remote Disconnected with: " << ec.message(); });
  } else {
    running_ = false;
    common::logger::LibBoostLogger::GetLibBoostLogger().GetLogger().LogError(
        __FILE__, __LINE__, __func__,
        [ec](std::stringstream &msg) { msg << "Remote Disconnected with undefined error: " << ec.message(); });
  }
}
}  // namespace tcp
}  // namespace socket
}  // namespace boost_support
