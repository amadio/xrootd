/******************************************************************************/
/*                                                                            */
/*                  X r d C l H t t p R e d i r e c t T e s t               */
/*                                                                            */
/* (c) 2026 by the XRootD Collaboration                                       */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with XRootD in a file called COPYING.LESSER (LGPL license) and file  */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/*                                                                            */
/******************************************************************************/

#include "XrdClHttp/XrdClHttpOps.hh"
#include "XrdClHttp/XrdClHttpUtil.hh"
#include "XrdClHttp/XrdClHttpWorker.hh"

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClLog.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

class RedirectServer {
public:
  explicit RedirectServer(std::string payload) : m_payload(std::move(payload)) {
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket == -1)
      return;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(m_socket, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) == -1 ||
        listen(m_socket, 2) == -1)
      return;

    socklen_t addressLength = sizeof(address);
    if (getsockname(m_socket, reinterpret_cast<sockaddr *>(&address),
                    &addressLength) == -1)
      return;

    m_port = ntohs(address.sin_port);
    m_thread = std::thread(&RedirectServer::Run, this);
  }

  ~RedirectServer() {
    if (m_socket != -1)
      close(m_socket);
    if (m_thread.joinable())
      m_thread.join();
  }

  bool IsReady() const { return m_port != 0; }
  uint16_t Port() const { return m_port; }

  std::string Url() const {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/redirect";
  }

private:
  static bool SendAll(int socket, const std::string &response) {
    std::size_t sent = 0;
    while (sent < response.size()) {
      const auto result =
          send(socket, response.data() + sent, response.size() - sent, 0);
      if (result <= 0)
        return false;
      sent += static_cast<std::size_t>(result);
    }
    return true;
  }

  static std::string ReadRequest(int socket) {
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto result = recv(socket, buffer, sizeof(buffer), 0);
      if (result <= 0)
        return {};
      request.append(buffer, static_cast<std::size_t>(result));
    }
    return request;
  }

  void Run() {
    for (unsigned request = 0; request < 3; ++request) {
      const auto client = accept(m_socket, nullptr, nullptr);
      if (client == -1)
        return;

      const auto requestData = ReadRequest(client);
      if (requestData.empty()) {
        close(client);
        return;
      }

      std::string response;
      if (requestData.find(" /poison ") != std::string::npos) {
        response = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
                   "Connection: close\r\n\r\nx";
      } else if (requestData.find(" /redirect ") != std::string::npos) {
        response = "HTTP/1.1 307 Temporary Redirect\r\n"
                   "Location: http://127.0.0.1:" +
                   std::to_string(m_port) +
                   "/data\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      } else {
        std::ostringstream chunkSize;
        chunkSize << std::hex << m_payload.size();
        response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                   "Connection: close\r\n\r\n" +
                   chunkSize.str() + "\r\n" + m_payload + "\r\n0\r\n\r\n";
      }

      SendAll(client, response);
      close(client);
    }
  }

  int m_socket{-1};
  uint16_t m_port{0};
  std::string m_payload;
  std::thread m_thread;
};

class RawReadOperation final : public XrdClHttp::CurlReadOp {
public:
  using CurlReadOp::CurlReadOp;

  // Leave raw transfer state on the handle before the worker returns it to
  // its thread-local pool.
  bool Setup(CURL *curl, XrdClHttp::CurlWorker &worker) override {
    return CurlReadOp::Setup(curl, worker) &&
           curl_easy_setopt(curl, CURLOPT_HTTP_TRANSFER_DECODING, 0L) ==
               CURLE_OK;
  }
};

class ResponseHandler final : public XrdCl::ResponseHandler {
public:
  void HandleResponse(XrdCl::XRootDStatus *status,
                      XrdCl::AnyObject *response) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.reset(status);
    m_response.reset(response);
    m_condition.notify_one();
  }

  bool Wait() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, std::chrono::seconds(10),
                                [this] { return m_status != nullptr; });
  }

  std::unique_ptr<XrdCl::XRootDStatus> m_status;
  std::unique_ptr<XrdCl::AnyObject> m_response;

private:
  std::mutex m_mutex;
  std::condition_variable m_condition;
};

TEST(Redirect, RestoresTransferDecodingForReusedHandle) {
  const std::string payload(1024 * 1024, 'x');
  RedirectServer server(payload);
  ASSERT_TRUE(server.IsReady());

  auto queue = std::make_shared<XrdClHttp::HandlerQueue>(2);
  auto &cache = XrdClHttp::VerbsCache::Instance();
  auto *logger = XrdCl::DefaultEnv::GetLog();
  auto worker = std::make_unique<XrdClHttp::CurlWorker>(queue, cache, logger);
  auto *workerPointer = worker.get();
  std::thread workerThread(XrdClHttp::CurlWorker::RunStatic, workerPointer);
  workerPointer->Start(std::move(worker), std::move(workerThread));

  ResponseHandler poisonHandler;
  char poisonBuffer = '\0';
  timespec timeout{10, 0};
  auto poisonOperation = std::make_shared<RawReadOperation>(
      &poisonHandler, nullptr,
      "http://127.0.0.1:" + std::to_string(server.Port()) + "/poison", timeout,
      std::make_pair<uint64_t, uint64_t>(0, 1), &poisonBuffer, 1, logger,
      nullptr, nullptr);
  queue->Produce(poisonOperation);
  ASSERT_TRUE(poisonHandler.Wait());
  ASSERT_NE(poisonHandler.m_status, nullptr);
  ASSERT_TRUE(poisonHandler.m_status->IsOK())
      << poisonHandler.m_status->ToString();

  ResponseHandler handler;
  std::string buffer(payload.size(), '\0');
  auto operation = std::make_shared<XrdClHttp::CurlReadOp>(
      &handler, nullptr, server.Url(), timeout,
      std::make_pair<uint64_t, uint64_t>(0, INT64_MAX), buffer.data(),
      buffer.size(), logger, nullptr, nullptr);
  queue->Produce(operation);

  ASSERT_TRUE(handler.Wait());
  ASSERT_NE(handler.m_status, nullptr);
  ASSERT_TRUE(handler.m_status->IsOK()) << handler.m_status->ToString();
  ASSERT_NE(handler.m_response, nullptr);

  XrdCl::ChunkInfo *chunk = nullptr;
  handler.m_response->Get(chunk);
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->GetLength(), payload.size());
  EXPECT_EQ(buffer.substr(0, 16), payload.substr(0, 16));
  EXPECT_EQ(buffer.substr(buffer.size() - 16),
            payload.substr(payload.size() - 16));
  EXPECT_TRUE(buffer == payload) << "Redirected chunked body was not decoded";
}

} // namespace
