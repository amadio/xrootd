/******************************************************************************/
/*                                                                            */
/*              X r d C l H t t p R e a d R e s p o n s e T e s t           */
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

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <memory>
#include <string>

namespace {
class ReadHandler : public XrdCl::ResponseHandler {
public:
  void HandleResponse(XrdCl::XRootDStatus *result, XrdCl::AnyObject *object) override
  {
    ++calls;
    status.reset(result);
    response.reset(object);
  }

  unsigned calls{0};
  std::unique_ptr<XrdCl::XRootDStatus> status;
  std::unique_ptr<XrdCl::AnyObject> response;
};

class ReadOperation : public XrdClHttp::CurlReadOp {
public:
  ReadOperation(ReadHandler &handler, uint64_t offset, char *buffer, size_t size)
    : CurlReadOp(&handler, nullptr, "http://localhost/file", {10, 0},
                 {offset, size}, buffer, size, XrdCl::DefaultEnv::GetLog(),
                 nullptr, nullptr) {}

  using CurlReadOp::Write;

  bool Header(const std::string &line) {return m_headers.Parse(line);}

  void CompleteHttpError()
  {
    auto error = XrdClHttp::HTTPStatusConvert(GetStatusCode());
    Fail(error.first, error.second, GetStatusMessage());
  }
};
}

TEST(ReadResponse, PreservesHttpErrorsAtNonzeroOffset)
{
  for (int code : {401, 403, 404, 424, 500}) {
    SCOPED_TRACE(code);
    ReadHandler handler;
    std::array<char, 4> buffer{'?', '?', '?', '?'};
    ReadOperation op(handler, 8, buffer.data(), buffer.size());
    ASSERT_TRUE(op.Header("HTTP/1.1 " + std::to_string(code) + " Error\r\n"));
    ASSERT_TRUE(op.Header("Content-Length: 10\r\n"));
    ASSERT_TRUE(op.Header("\r\n"));
    char body[] = "read error";
    EXPECT_EQ(op.Write(body, 10), 10);
    EXPECT_EQ(op.GetError(), XrdClHttp::CurlOperation::ErrNone);
    EXPECT_EQ(buffer, (std::array<char, 4>{'?', '?', '?', '?'}));
    op.CompleteHttpError();
    ASSERT_EQ(handler.calls, 1);
    ASSERT_TRUE(handler.status);
    EXPECT_FALSE(handler.status->IsOK());
    auto expected = XrdClHttp::HTTPStatusConvert(code);
    EXPECT_EQ(handler.status->code, expected.first);
    EXPECT_EQ(handler.status->errNo, expected.second);
    EXPECT_TRUE(op.HasFailed());
  }
}

TEST(ReadResponse, ReturnsEmptyReadOnlyForConfirmedEof)
{
  for (auto offsets : {std::pair<uint64_t, uint64_t>{32, 32}, {40, 32}, {0, 0}}) {
    SCOPED_TRACE(offsets.first);
    ReadHandler handler;
    std::array<char, 4> buffer{'?', '?', '?', '?'};
    ReadOperation op(handler, offsets.first, buffer.data(), buffer.size());
    ASSERT_TRUE(op.Header("HTTP/1.1 416 Range Not Satisfiable\r\n"));
    ASSERT_TRUE(op.Header("Content-Range: bytes */" + std::to_string(offsets.second) + "\r\n"));
    ASSERT_TRUE(op.Header("\r\n"));
    char body[] = "outside file";
    ASSERT_EQ(op.Write(body, 12), 12);
    op.CompleteHttpError();
    ASSERT_EQ(handler.calls, 1);
    ASSERT_TRUE(handler.status);
    EXPECT_TRUE(handler.status->IsOK());
    EXPECT_TRUE(op.IsDone());
    EXPECT_FALSE(op.HasFailed());
    ASSERT_TRUE(handler.response);
    XrdCl::ChunkInfo *chunk = nullptr;
    handler.response->Get(chunk);
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->length, 0);
    EXPECT_EQ(chunk->offset, offsets.first);
    EXPECT_EQ(buffer, (std::array<char, 4>{'?', '?', '?', '?'}));
  }
}

TEST(ReadResponse, KeepsUnconfirmedRangeErrors)
{
  for (auto test : {std::pair<uint64_t, const char *>{32, ""},
                    {8, "Content-Range: bytes */32\r\n"}}) {
    auto range = test.second;
    SCOPED_TRACE(range);
    ReadHandler handler;
    char buffer[4]{};
    ReadOperation op(handler, test.first, buffer, sizeof(buffer));
    ASSERT_TRUE(op.Header("HTTP/1.1 416 Range Not Satisfiable\r\n"));
    if (*range) ASSERT_TRUE(op.Header(range));
    ASSERT_TRUE(op.Header("\r\n"));
    op.CompleteHttpError();
    ASSERT_TRUE(handler.status);
    EXPECT_FALSE(handler.status->IsOK());
    EXPECT_EQ(handler.status->errNo, kXR_InvalidRequest);
    EXPECT_TRUE(op.HasFailed());
    EXPECT_FALSE(handler.response);
  }
}

TEST(ReadResponse, DoesNotHideTransportFailureAtEof)
{
  ReadHandler handler;
  char buffer[4]{};
  ReadOperation op(handler, 32, buffer, sizeof(buffer));
  ASSERT_TRUE(op.Header("HTTP/1.1 416 Range Not Satisfiable\r\n"));
  ASSERT_TRUE(op.Header("Content-Range: bytes */32\r\n"));
  op.Fail(XrdCl::errSocketError, EIO, "Connection failed");
  ASSERT_TRUE(handler.status);
  EXPECT_FALSE(handler.status->IsOK());
  EXPECT_EQ(handler.status->code, XrdCl::errSocketError);
  EXPECT_TRUE(op.HasFailed());
}

TEST(ReadResponse, ReadsSatisfiedRange)
{
  ReadHandler handler;
  char buffer[4]{};
  ReadOperation op(handler, 8, buffer, sizeof(buffer));
  ASSERT_TRUE(op.Header("HTTP/1.1 206 Partial Content\r\n"));
  ASSERT_TRUE(op.Header("Content-Range: bytes 8-11/32\r\n"));
  ASSERT_TRUE(op.Header("\r\n"));
  char body[] = "data";
  ASSERT_EQ(op.Write(body, 4), 4);
  op.Success();
  ASSERT_TRUE(handler.status);
  EXPECT_TRUE(handler.status->IsOK());
  EXPECT_EQ(std::string(buffer, 4), "data");
}

TEST(ReadResponse, RejectsSuccessfulResponseWithWrongOffset)
{
  ReadHandler handler;
  char buffer[4]{};
  ReadOperation op(handler, 8, buffer, sizeof(buffer));
  ASSERT_TRUE(op.Header("HTTP/1.1 200 OK\r\n"));
  ASSERT_TRUE(op.Header("\r\n"));
  char body[] = "data";
  EXPECT_EQ(op.Write(body, 4), 0);
  EXPECT_EQ(op.GetError(), XrdClHttp::CurlOperation::ErrCallback);
  EXPECT_EQ(op.GetCallbackError().first, kXR_ServerError);
}
