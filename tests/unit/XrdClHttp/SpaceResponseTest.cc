/******************************************************************************/
/* Copyright (C) 2026, XRootD Collaboration                                  */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/******************************************************************************/

#include "XrdClHttp/XrdClHttpOps.hh"
#include "XrdClHttp/XrdClHttpWorker.hh"

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClUtils.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

struct Exchange {
    std::string request;
    std::string response;
    bool failed{false};
};

static ssize_t WriteAll(int fd, const char *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t res = write(fd, data + written, size - written);
        if (res < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += res;
    }
    return written;
}

class HttpHandler final {
public:
    explicit HttpHandler(Exchange &exchange) : m_exchange(exchange) {}

    void HandleConnection(int socket) {
        XrdCl::ScopedDescriptor descriptor(socket);
        char buffer[4096];
        size_t expected = std::string::npos;
        while (expected == std::string::npos ||
               m_exchange.request.size() < expected) {
            ssize_t received;
            do { received = read(socket, buffer, sizeof(buffer)); }
            while (received < 0 && errno == EINTR);
            if (received <= 0 ||
                m_exchange.request.size() + received > 64 * 1024) {
                m_exchange.failed = true;
                return;
            }
            m_exchange.request.append(buffer, received);
            const auto end = m_exchange.request.find("\r\n\r\n");
            if (end != std::string::npos) {
                expected = end + 4;
                const auto length = m_exchange.request.find("Content-Length: ");
                if (length < end)
                    expected += std::stoul(m_exchange.request.substr(length + 16));
            }
        }
        if (WriteAll(socket, m_exchange.response.data(),
                m_exchange.response.size()) !=
            static_cast<ssize_t>(m_exchange.response.size()))
            m_exchange.failed = true;
    }

private:
    Exchange &m_exchange;
};

class SimpleHttpServer final {
public:
    explicit SimpleHttpServer(std::array<Exchange, 2> &exchanges)
        : m_exchanges(exchanges) {}

    ~SimpleHttpServer() { Stop(); }

    bool Start() {
        m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
            close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        m_port = ntohs(addr.sin_port);
        if (listen(m_listen_fd, 2) < 0) {
            close(m_listen_fd);
            m_listen_fd = -1;
            return false;
        }
        m_thread = std::thread([this]() {
            for (auto &exchange : m_exchanges) {
                int client_fd = accept(m_listen_fd, nullptr, nullptr);
                if (client_fd < 0) return;
                HttpHandler handler(exchange);
                handler.HandleConnection(client_fd);
            }
        });
        return true;
    }

    bool Stop() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_listen_fd >= 0) {
            close(m_listen_fd);
            m_listen_fd = -1;
        }
        return true;
    }

    int GetPort() const { return m_port; }

private:
    std::array<Exchange, 2> &m_exchanges;
    int m_listen_fd{-1};
    int m_port{0};
    std::thread m_thread;
};

class ResponseHandler final : public XrdCl::ResponseHandler {
public:
    void HandleResponse(XrdCl::XRootDStatus *status,
                        XrdCl::AnyObject *response) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.reset(status);
        m_response.reset(response);
        m_ready.notify_one();
    }

    bool Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_ready.wait_for(lock, std::chrono::seconds(10),
            [this] { return m_status != nullptr; });
    }

    std::unique_ptr<XrdCl::XRootDStatus> m_status;
    std::unique_ptr<XrdCl::AnyObject> m_response;

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
};

std::string WireResponse(const std::string &status, const std::string &body,
                         const std::string &headers = "") {
    return "HTTP/1.1 " + status + "\r\n" + headers +
        "Content-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
}

}

TEST(SpaceResponse, RestoresGetForPooledReadAfterQuotaQuery)
{
    const std::string quota =
        "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:propstat>"
        "<d:status>HTTP/1.1 200 OK</d:status><d:prop>"
        "<d:quota-used-bytes>17</d:quota-used-bytes>"
        "<d:quota-available-bytes>83</d:quota-available-bytes>"
        "</d:prop></d:propstat></d:response></d:multistatus>";
    std::array<Exchange, 2> exchanges;
    exchanges[0].response = WireResponse("207 Multi-Status", quota);
    exchanges[1].response = WireResponse("206 Partial Content", "data",
        "Content-Range: bytes 0-3/4\r\n");
    SimpleHttpServer server(exchanges);
    ASSERT_TRUE(server.Start());
    const std::string endpoint =
        "http://127.0.0.1:" + std::to_string(server.GetPort());

    // One worker and sequential operations force the read to reuse the quota
    // request's easy handle through the real handler pool.
    auto queue = std::make_shared<XrdClHttp::HandlerQueue>(2);
    auto *logger = XrdCl::DefaultEnv::GetLog();
    auto worker = std::make_unique<XrdClHttp::CurlWorker>(
        queue, XrdClHttp::VerbsCache::Instance(), logger);
    auto *worker_pointer = worker.get();
    std::thread thread(XrdClHttp::CurlWorker::RunStatic, worker_pointer);
    worker_pointer->Start(std::move(worker), std::move(thread));

    ResponseHandler quota_handler;
    queue->Produce(std::make_shared<XrdClHttp::CurlSpaceOp>(
        &quota_handler, endpoint + "/quota", timespec{5, 0}, logger,
        nullptr, nullptr));
    ASSERT_TRUE(quota_handler.Wait());
    ASSERT_TRUE(quota_handler.m_status->IsOK())
        << quota_handler.m_status->ToString();
    XrdCl::Buffer *result = nullptr;
    ASSERT_NE(quota_handler.m_response, nullptr);
    quota_handler.m_response->Get(result);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->ToString(),
        "oss.space=100&oss.free=83&oss.used=17&oss.maxf=83");

    ResponseHandler read_handler;
    std::array<char, 4> buffer{};
    queue->Produce(std::make_shared<XrdClHttp::CurlReadOp>(
        &read_handler, nullptr, endpoint + "/read", timespec{5, 0},
        std::make_pair<uint64_t, uint64_t>(0, buffer.size()),
        buffer.data(), buffer.size(), logger, nullptr, nullptr));
    ASSERT_TRUE(read_handler.Wait());
    ASSERT_TRUE(server.Stop());
    EXPECT_TRUE(read_handler.m_status->IsOK())
        << read_handler.m_status->ToString();
    EXPECT_EQ(std::string(buffer.data(), buffer.size()), "data");
    EXPECT_EQ(exchanges[0].request.substr(0, exchanges[0].request.find("\r\n")),
        "PROPFIND /quota HTTP/1.1");
    EXPECT_EQ(exchanges[1].request.substr(0, exchanges[1].request.find("\r\n")),
        "GET /read HTTP/1.1");
    EXPECT_NE(exchanges[1].request.find("Range: bytes=0-3\r\n"),
              std::string::npos);
    EXPECT_FALSE(exchanges[0].failed);
    EXPECT_FALSE(exchanges[1].failed);
}
