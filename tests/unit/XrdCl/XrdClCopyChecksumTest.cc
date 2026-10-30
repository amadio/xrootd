//------------------------------------------------------------------------------
// Copyright (c) 2026 by European Organization for Nuclear Research (CERN)
//------------------------------------------------------------------------------
// This file is part of the XRootD software suite.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//
// In applying this licence, CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
//------------------------------------------------------------------------------

#include "XrdCl/XrdClCopyChecksum.hh"

#include <gtest/gtest.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
  class CopyChecksumTest: public ::testing::Test
  {
    protected:
      void SetUp() override
      {
        char pattern[] = "/tmp/xrdcl-copy-checksum-XXXXXX";
        char *path = mkdtemp( pattern );
        ASSERT_NE( path, nullptr );
        directory = path;
        source = directory + "/source file";
        target = directory + "/target file";
        Write( source, "hello" );
        Write( target, "original" );
        properties.Set( "source", source );
        properties.Set( "target", target );
        properties.Set( "checkSumVerify", "both" );
        properties.Set( "checkSumType", "adler32" );
      }

      void TearDown() override
      {
        if( directory.empty() ) return;
        unlink( source.c_str() );
        unlink( target.c_str() );
        unlink( ( directory + "/alias" ).c_str() );
        unlink( ( directory + "/fifo" ).c_str() );
        rmdir( directory.c_str() );
      }

      void Write( const std::string &path, const std::string &value )
      {
        std::ofstream stream( path.c_str(), std::ios::binary | std::ios::trunc );
        ASSERT_TRUE( stream.good() );
        stream << value;
        ASSERT_TRUE( stream.good() );
      }

      std::string Read( const std::string &path )
      {
        std::ifstream stream( path.c_str(), std::ios::binary );
        return std::string( std::istreambuf_iterator<char>( stream ),
                            std::istreambuf_iterator<char>() );
      }

      std::string directory, source, target;
      XrdCl::PropertyList properties, results;
  };
}

TEST_F( CopyChecksumTest, PreserveLegacyChecksumPolicy )
{
  XrdCl::PropertyList legacy;
  legacy.Set( "checkSumMode", "end2end" );
  legacy.Set( "checkSumType", "auto" );
  legacy.Set( "source", "stdio://-" );
  ASSERT_TRUE( XrdCl::ValidateCopyChecksum( legacy ).IsOK() );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( legacy, results ).IsOK() );
  ASSERT_TRUE( XrdCl::FinishCopyChecksum( legacy, results, XrdCl::XRootDStatus() ).IsOK() );
  EXPECT_EQ( legacy.Get<std::string>( "checkSumMode" ), "end2end" );
  EXPECT_EQ( legacy.Get<std::string>( "checkSumType" ), "auto" );
  EXPECT_FALSE( results.HasProperty( "sourceCheckSum" ) );
}

TEST_F( CopyChecksumTest, ValidateWithoutOpeningFiles )
{
  properties.Set( "source", directory + "/missing source" );
  properties.Set( "target", directory + "/missing target" );
  properties.Set( "checkSumVerify", "source" );
  properties.Set( "checkSumPreset", "62C0215" );
  EXPECT_TRUE( XrdCl::ValidateCopyChecksum( properties ).IsOK() );
  EXPECT_FALSE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  EXPECT_NE( access( ( directory + "/missing target" ).c_str(), F_OK ), 0 );
}

TEST_F( CopyChecksumTest, SourceMismatchPreservesExistingDestination )
{
  properties.Set( "force", true );
  properties.Set( "rmOnBadCksum", true );
  properties.Set( "checkSumPreset", "1" );
  XrdCl::XRootDStatus status = XrdCl::PrepareCopyChecksum( properties, results );
  EXPECT_EQ( status.code, XrdCl::errCheckSumError );
  EXPECT_EQ( status.errNo, EDOM );
  EXPECT_EQ( results.Get<std::string>( "sourceCheckSum" ), "adler32:062c0215" );
  EXPECT_EQ( Read( target ), "original" );
  EXPECT_FALSE( results.HasProperty( "targetCheckSum" ) );
}

TEST_F( CopyChecksumTest, CanonicalizeExpectedChecksumBeforeCopy )
{
  properties.Set( "checkSumType", "ADLER32" );
  properties.Set( "checkSumVerify", "source" );
  properties.Set( "checkSumPreset", "62C0215" );
  properties.Set( "checkSumMode", "end2end" );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  EXPECT_EQ( properties.Get<std::string>( "checkSumMode" ), "none" );
  EXPECT_EQ( properties.Get<std::string>( "checkSumType" ), "adler32" );
  EXPECT_EQ( properties.Get<std::string>( "checkSumVerifyExpected" ), "adler32:062c0215" );
  EXPECT_EQ( properties.Get<std::string>( "checkSumPreset" ), "62C0215" );
  EXPECT_TRUE( XrdCl::FinishCopyChecksum( properties, results, XrdCl::XRootDStatus() ).IsOK() );
  EXPECT_FALSE( results.HasProperty( "targetCheckSum" ) );
}

TEST_F( CopyChecksumTest, ComparePreflightSourceToCompletedTarget )
{
  properties.Set( "checkSumType", "auto" );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  EXPECT_EQ( properties.Get<std::string>( "checkSumType" ), "adler32" );
  Write( target, "hello" );
  EXPECT_TRUE( XrdCl::FinishCopyChecksum( properties, results, XrdCl::XRootDStatus() ).IsOK() );
  EXPECT_EQ( results.Get<std::string>( "sourceCheckSum" ), "adler32:062c0215" );
  EXPECT_EQ( results.Get<std::string>( "targetCheckSum" ), "adler32:062c0215" );
}

TEST_F( CopyChecksumTest, TargetOnlyDoesNotClaimSourceChecksum )
{
  properties.Set( "checkSumVerify", "target" );
  properties.Set( "checkSumPreset", "062c0215" );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  EXPECT_FALSE( results.HasProperty( "sourceCheckSum" ) );
  Write( target, "hello" );
  EXPECT_TRUE( XrdCl::FinishCopyChecksum( properties, results, XrdCl::XRootDStatus() ).IsOK() );
  EXPECT_EQ( results.Get<std::string>( "targetCheckSum" ), "adler32:062c0215" );
}

TEST_F( CopyChecksumTest, PreserveTargetOnMismatchByDefault )
{
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  XrdCl::XRootDStatus status = XrdCl::FinishCopyChecksum( properties, results, XrdCl::XRootDStatus() );
  EXPECT_EQ( status.code, XrdCl::errCheckSumError );
  EXPECT_EQ( status.errNo, EDOM );
  EXPECT_EQ( Read( target ), "original" );
  EXPECT_TRUE( results.HasProperty( "targetCheckSum" ) );
}

TEST_F( CopyChecksumTest, RemoveTargetOnMismatchOnlyWhenRequested )
{
  properties.Set( "rmOnBadCksum", true );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  XrdCl::XRootDStatus status = XrdCl::FinishCopyChecksum( properties, results, XrdCl::XRootDStatus() );
  EXPECT_EQ( status.code, XrdCl::errCheckSumError );
  EXPECT_NE( access( target.c_str(), F_OK ), 0 );
  EXPECT_TRUE( results.Get<XrdCl::XRootDStatus>( "checkSumRemovalStatus" ).IsOK() );
}

TEST_F( CopyChecksumTest, FailedTransferNeverQueriesOrRemovesTarget )
{
  properties.Set( "rmOnBadCksum", true );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  XrdCl::XRootDStatus failure( XrdCl::stError, XrdCl::errOperationInterrupted, EINTR );
  XrdCl::XRootDStatus status = XrdCl::FinishCopyChecksum( properties, results, failure );
  EXPECT_EQ( status.code, failure.code );
  EXPECT_EQ( status.errNo, failure.errNo );
  EXPECT_EQ( Read( target ), "original" );
  EXPECT_FALSE( results.HasProperty( "targetCheckSum" ) );
}

TEST_F( CopyChecksumTest, RequireExpectedDigestForOneSidedModes )
{
  for( const char *mode: { "source", "target" } )
  {
    properties.Set( "checkSumVerify", mode );
    XrdCl::XRootDStatus status = XrdCl::ValidateCopyChecksum( properties );
    EXPECT_EQ( status.code, XrdCl::errInvalidArgs );
  }
}

TEST_F( CopyChecksumTest, RejectMalformedChecksumBeforeWriting )
{
  for( const char *digest: { "not-hex", "abcdef012", "1;rm", " adler", "0\n1" } )
  {
    properties.Set( "checkSumPreset", digest );
    EXPECT_EQ( XrdCl::PrepareCopyChecksum( properties, results ).code, XrdCl::errInvalidArgs );
    EXPECT_EQ( Read( target ), "original" );
  }
  properties.Set( "checkSumPreset", "00000000" );
  for( const char *type: { "adler32&authz=bad", "../md5", "", "2md5", "adler32 bad" } )
  {
    properties.Set( "checkSumType", type );
    EXPECT_EQ( XrdCl::ValidateCopyChecksum( properties ).code, XrdCl::errInvalidArgs );
  }
}

TEST_F( CopyChecksumTest, RequireFullCryptographicDigest )
{
  properties.Set( "checkSumType", "md5" );
  properties.Set( "checkSumPreset", "5d41402abc4b2a76b9719d911017c59" );
  EXPECT_EQ( XrdCl::ValidateCopyChecksum( properties ).code, XrdCl::errInvalidArgs );
  properties.Set( "checkSumPreset", "5D41402ABC4B2A76B9719D911017C592" );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  EXPECT_EQ( results.Get<std::string>( "sourceCheckSum" ), "md5:5d41402abc4b2a76b9719d911017c592" );
}

TEST_F( CopyChecksumTest, HandleZeroIntegerChecksum )
{
  Write( source, "" );
  Write( target, "" );
  // crc32 is built in, unlike the optional zcrc32 plugin. Its native helper
  // elides every zero from an empty-file digest, which must still compare to 0.
  properties.Set( "checkSumType", "crc32" );
  properties.Set( "checkSumPreset", "0" );
  ASSERT_TRUE( XrdCl::PrepareCopyChecksum( properties, results ).IsOK() );
  EXPECT_EQ( results.Get<std::string>( "sourceCheckSum" ), "crc32:00000000" );
  EXPECT_TRUE( XrdCl::FinishCopyChecksum( properties, results, XrdCl::XRootDStatus() ).IsOK() );
}

TEST_F( CopyChecksumTest, RejectUnsupportedModesAndJobKinds )
{
  properties.Set( "checkSumVerify", "end2end" );
  EXPECT_EQ( XrdCl::ValidateCopyChecksum( properties ).code, XrdCl::errInvalidArgs );
  properties.Set( "checkSumVerify", "both" );
  for( const char *name: { "zipArchive", "zipAppend", "dynamicSource", "recursive" } )
  {
    properties.Set( name, true );
    EXPECT_EQ( XrdCl::ValidateCopyChecksum( properties ).errNo, ENOTSUP );
    properties.Set( name, false );
  }
  properties.Set( "source", "stdio://-" );
  EXPECT_EQ( XrdCl::ValidateCopyChecksum( properties ).errNo, ENOTSUP );
}

TEST_F( CopyChecksumTest, RejectFifoWithoutBlocking )
{
  std::string fifo = directory + "/fifo";
  ASSERT_EQ( mkfifo( fifo.c_str(), 0600 ), 0 );
  properties.Set( "source", fifo );
  EXPECT_EQ( XrdCl::PrepareCopyChecksum( properties, results ).errNo, ENOTSUP );
  properties.Set( "source", source );
  properties.Set( "target", fifo );
  EXPECT_EQ( XrdCl::PrepareCopyChecksum( properties, results ).errNo, ENOTSUP );
}

TEST_F( CopyChecksumTest, RejectHardLinkedSourceAndTarget )
{
  std::string alias = directory + "/alias";
  ASSERT_EQ( link( source.c_str(), alias.c_str() ), 0 );
  properties.Set( "target", alias );
  EXPECT_EQ( XrdCl::PrepareCopyChecksum( properties, results ).code, XrdCl::errInvalidArgs );
  EXPECT_EQ( Read( source ), "hello" );
}
