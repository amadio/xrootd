// Copyright (c) 2026 by the XRootD developers.
// Distributed under the GNU Lesser General Public License, version 3 or later.
#include <gtest/gtest.h>
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClPlugInManager.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include <deque>
#include <memory>
#include <thread>

namespace
{
  using namespace XrdCl;
  struct Pending { ChunkList chunks; ResponseHandler *handler; };
  struct State
  {
    std::string reply = "2 4";
    bool automatic = true, rejectQuery = false;
    int reject = -1, malformed = 0, submitted = 0, queries = 0;
    size_t peak = 0;
    std::deque<Pending> pending;
    std::vector<ChunkList> batches;
    void Complete()
    {
      auto request = pending.front();
      pending.pop_front();
      auto info = new VectorReadInfo();
      info->GetChunks() = request.chunks;
      uint32_t size = 0;
      for( auto chunk : request.chunks )
      {
        for( uint32_t i = 0; i < chunk.length; ++i )
          static_cast<char *>( chunk.buffer )[i] = (chunk.offset + i) % 127;
        size += chunk.length;
      }
      info->SetSize( size );
      if( malformed == 1 ) info->GetChunks().clear();
      if( malformed == 2 ) ++info->GetChunks()[0].offset;
      if( malformed == 3 ) --info->GetChunks()[0].length;
      auto response = new AnyObject();
      response->Set( info );
      request.handler->HandleResponse( new XRootDStatus(), response );
    }
  };
  class MockFile : public FilePlugIn
  {
    public:
      MockFile( std::shared_ptr<State> state, std::string url ): state( state ), url( url ) {}
      bool IsOpen() const override { return true; }
      bool GetProperty( const std::string &name, std::string &value ) const override
      { if( name != "DataServer" ) return false; value = url; return true; }
      XRootDStatus VectorRead( const ChunkList &chunks, void *, ResponseHandler *handler,
                               time_t ) override
      {
        state->batches.push_back( chunks );
        if( state->submitted++ == state->reject ) return XRootDStatus( stError, errInvalidOp );
        state->pending.push_back( {chunks, handler} );
        state->peak = std::max( state->peak, state->pending.size() );
        if( state->automatic ) state->Complete();
        return XRootDStatus();
      }
    private:
      std::shared_ptr<State> state;
      std::string url;
  };
  class MockFS : public FileSystemPlugIn
  {
    public:
      explicit MockFS( std::shared_ptr<State> state ): state( state ) {}
      XRootDStatus Query( QueryCode::Code, const Buffer &, ResponseHandler *handler,
                          time_t ) override
      {
        ++state->queries;
        if( state->rejectQuery ) return XRootDStatus( stError, errNotImplemented );
        auto buffer = new Buffer();
        buffer->FromString( state->reply );
        auto response = new AnyObject();
        response->Set( buffer );
        handler->HandleResponse( new XRootDStatus(), response );
        return XRootDStatus();
      }
    private:
      std::shared_ptr<State> state;
  };
  class Factory : public PlugInFactory
  {
    public:
      explicit Factory( std::shared_ptr<State> state ): state( state ) {}
      FilePlugIn *CreateFile( const std::string &url ) override { return new MockFile( state, url ); }
      FileSystemPlugIn *CreateFileSystem( const std::string & ) override { return new MockFS( state ); }
    private:
      std::shared_ptr<State> state;
  };
  class Completion : public ResponseHandler
  {
    public:
      void HandleResponse( XRootDStatus *value, AnyObject *response ) override
      { ++calls; status = *value; delete value; delete response; }
      unsigned calls = 0;
      XRootDStatus status;
  };
  class ReadRangesTest : public ::testing::Test
  {
    protected:
      void SetUp() override
      {
        static unsigned next = 0;
        state = std::make_shared<State>();
        url = "root://ranges-" + std::to_string( ++next ) + ".invalid/";
        ASSERT_TRUE( DefaultEnv::GetPlugInManager()->RegisterDefaultFactory( new Factory( state ) ) );
      }
      void TearDown() override
      { DefaultEnv::GetPlugInManager()->RegisterDefaultFactory( nullptr ); }
      std::shared_ptr<State> state;
      std::string url;
  };
}

TEST_F( ReadRangesTest, SplitAssembleAndCacheLimits )
{
  File file( url );
  char first[25], second[3];
  ChunkList chunks{ {17, sizeof(first), first}, {90, 0, nullptr}, {4, sizeof(second), second} };
  ASSERT_TRUE( file.ReadRanges( chunks, 2 ).IsOK() );
  for( size_t i = 0; i < sizeof(first); ++i ) EXPECT_EQ( first[i], (17 + i) % 127 );
  for( size_t i = 0; i < sizeof(second); ++i ) EXPECT_EQ( second[i], (4 + i) % 127 );
  ASSERT_TRUE( file.ReadRanges( chunks, 2 ).IsOK() );
  EXPECT_EQ( state->queries, 1 );
  for( const auto &batch : state->batches )
  {
    EXPECT_LE( batch.size(), 2u );
    for( const auto &chunk : batch ) EXPECT_LE( chunk.length, 4u );
  }
}

TEST_F( ReadRangesTest, BoundConcurrencyAndDrainSubmissionFailure )
{
  state->automatic = false;
  File file( url );
  char data[40];
  Completion completion;
  ASSERT_TRUE( file.ReadRanges( {{0, sizeof(data), data}}, &completion, 2 ).IsOK() );
  EXPECT_EQ( state->pending.size(), 2u );
  while( !state->pending.empty() ) state->Complete();
  EXPECT_EQ( state->peak, 2u );
  EXPECT_EQ( completion.calls, 1u );
  EXPECT_TRUE( completion.status.IsOK() );
  state->submitted = 0;
  state->reject = 1;
  Completion failure;
  ASSERT_TRUE( file.ReadRanges( {{0, sizeof(data), data}}, &failure, 2 ).IsOK() );
  EXPECT_EQ( failure.calls, 0u );
  ASSERT_EQ( state->pending.size(), 1u );
  state->Complete();
  EXPECT_EQ( failure.calls, 1u );
  EXPECT_FALSE( failure.status.IsOK() );
  EXPECT_TRUE( state->pending.empty() );
}

TEST_F( ReadRangesTest, ValidateRepliesAndFallbackWhenDiscoveryFails )
{
  File file( url );
  char data[25];
  for( int malformed : {1, 2, 3} )
  {
    state->malformed = malformed;
    EXPECT_EQ( file.ReadRanges( {{0, sizeof(data), data}} ).code, errDataError );
  }
  state->malformed = 0;
  EXPECT_TRUE( file.ReadRanges( {} ).IsOK() );
  EXPECT_FALSE( file.ReadRanges( {{0, 1, nullptr}} ).IsOK() );
  EXPECT_FALSE( file.ReadRanges( {{UINT64_MAX, 1, data}} ).IsOK() );
  EXPECT_FALSE( file.ReadRanges( {}, uint16_t(0) ).IsOK() );
}

TEST_F( ReadRangesTest, InvalidLimitsAreNotCached )
{
  File file( url );
  char data[25];
  for( const auto &reply : {"0 4", "4 0", "garbage", "-1 4", "2 4 trailing"} )
  {
    state->reply = reply;
    ASSERT_TRUE( file.ReadRanges( {{0, sizeof(data), data}} ).IsOK() );
    EXPECT_EQ( state->batches.back().size(), 1u );
  }
  EXPECT_EQ( state->queries, 5 );
  state->rejectQuery = true;
  EXPECT_TRUE( file.ReadRanges( {{0, sizeof(data), data}} ).IsOK() );
  EXPECT_EQ( state->queries, 6 );
}

TEST_F( ReadRangesTest, TimeoutAppliesAcrossBatches )
{
  state->automatic = false;
  File file( url );
  char data[25];
  Completion completion;
  ASSERT_TRUE( file.ReadRanges( {{0, sizeof(data), data}}, &completion, 1, 1 ).IsOK() );
  std::this_thread::sleep_for( std::chrono::milliseconds( 1100 ) );
  state->Complete();
  EXPECT_EQ( completion.calls, 1u );
  EXPECT_EQ( completion.status.code, errOperationExpired );
  EXPECT_TRUE( state->pending.empty() );
}
