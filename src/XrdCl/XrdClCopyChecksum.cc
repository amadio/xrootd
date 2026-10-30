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
#include "XrdCl/XrdClCheckSumManager.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClUtils.hh"
#include "XrdCks/XrdCksCalc.hh"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
  std::string LowerCase( std::string value )
  {
    std::transform( value.begin(), value.end(), value.begin(),
                    []( unsigned char c )
                    { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; } );
    return value;
  }

  XrdCl::XRootDStatus InvalidChecksum( const std::string &message,
                                      int               error = EINVAL )
  {
    return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errInvalidArgs,
                               error, message );
  }

  bool ValidType( const std::string &type )
  {
    if( type.empty() || type.size() >= XrdCksData::NameSize ||
        type[0] < 'a' || type[0] > 'z' ) return false;
    return std::all_of( type.begin(), type.end(), []( unsigned char c )
                       { return ( c >= 'a' && c <= 'z' ) ||
                                ( c >= '0' && c <= '9' ) ||
                                c == '_' || c == '-'; } );
  }

  bool IntegerChecksum( const std::string &type )
  {
    return type == "adler32" || type == "crc32" || type == "zcrc32" ||
           type == "crc32c";
  }

  size_t ChecksumWidth( const std::string &type )
  {
    if( IntegerChecksum( type ) ) return 8;
    if( type == "md5" ) return 32;
    if( type == "sha1" ) return 40;
    if( type == "sha256" ) return 64;
    if( type == "sha512" ) return 128;
    return 0;
  }

  // The native helpers elide leading zeroes for some integer algorithms. Keep
  // a fixed width here so zero, abbreviated values and upper case all compare
  // consistently. Cryptographic digests must retain their exact width.
  bool NormalizeDigest( const std::string &type, std::string &digest,
                        size_t width, bool actual = false )
  {
    digest = LowerCase( digest );
    if( digest.empty() && !( actual && IntegerChecksum( type ) ) ) return false;
    if( digest.size() > 2 * XrdCksData::ValuSize ) return false;
    if( !std::all_of( digest.begin(), digest.end(), []( unsigned char c )
                     { return ( c >= '0' && c <= '9' ) ||
                              ( c >= 'a' && c <= 'f' ); } ) ) return false;
    if( IntegerChecksum( type ) )
    {
      if( digest.size() > width ) return false;
      digest.insert( 0, width - digest.size(), '0' );
    }
    else if( width ? digest.size() != width : digest.size() % 2 )
      return false;
    return true;
  }

  XrdCl::XRootDStatus RegularFile( const XrdCl::URL &url,
                                  const char        *side,
                                  bool               mustExist,
                                  struct stat       *info = 0 )
  {
    if( !url.IsLocalFile() ) return XrdCl::XRootDStatus();
    struct stat data;
    if( stat( url.GetPath().c_str(), &data ) != 0 )
    {
      int error = errno;
      if( !mustExist && error == ENOENT ) return XrdCl::XRootDStatus();
      return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errOSError, error,
                                 std::string( "Cannot inspect checksum " ) + side );
    }
    if( !S_ISREG( data.st_mode ) )
      return InvalidChecksum( std::string( "Explicit checksum verification " ) +
                              "requires a regular " + side + " file", ENOTSUP );
    if( info ) *info = data;
    return XrdCl::XRootDStatus();
  }

  XrdCl::XRootDStatus Calculate( const XrdCl::URL &url,
                                const std::string &type,
                                std::string       &checksum )
  {
    XrdCl::XRootDStatus status;
    if( url.IsLocalFile() )
    {
      errno = 0;
      status = XrdCl::Utils::GetLocalCheckSum( checksum, type, url.GetPath() );
      if( !status.IsOK() && status.code == XrdCl::errCheckSumError && !status.errNo )
        status.errNo = errno ? errno : EIO;
    }
    else
    {
      // The checksum query path must retain opaque URL arguments, including
      // authorization tokens. Utils::GetRemoteCheckSum appends cks.type to
      // GetPath(), so include the original arguments there for this query.
      XrdCl::URL queryURL( url );
      queryURL.SetPath( url.GetPathWithParams() );
      queryURL.SetParams( XrdCl::URL::ParamsMap() );
      status = XrdCl::Utils::GetRemoteCheckSum( checksum, type, queryURL );
    }
    if( !status.IsOK() ) return status;
    size_t delimiter = checksum.find( ':' );
    if( delimiter == std::string::npos ||
        LowerCase( checksum.substr( 0, delimiter ) ) != type )
      return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errCheckSumError,
                                 EINVAL, "Unexpected checksum algorithm in response" );
    std::string digest = checksum.substr( delimiter + 1 );
    if( !NormalizeDigest( type, digest, ChecksumWidth( type ), true ) )
      return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errCheckSumError,
                                 EINVAL, "Invalid checksum digest in response" );
    checksum = type + ':' + digest;
    return XrdCl::XRootDStatus();
  }

  bool Enabled( const XrdCl::PropertyList &properties, const char *name )
  {
    bool value = false;
    properties.Get( name, value );
    return value;
  }

  // A checksum mismatch only permits removal after a completed transfer, and
  // only when the user requested it. For local files, avoid unlinking a path
  // that was replaced while its checksum was being calculated.
  XrdCl::XRootDStatus RemoveTarget( const XrdCl::URL &target,
                                   const struct stat &checked )
  {
    if( target.IsLocalFile() )
    {
      struct stat current;
      if( stat( target.GetPath().c_str(), &current ) != 0 )
        return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errOSError, errno );
      if( current.st_dev != checked.st_dev || current.st_ino != checked.st_ino )
        return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errCheckSumError,
                                   EBUSY, "Checksum target changed; refusing removal" );
      if( unlink( target.GetPath().c_str() ) != 0 )
        return XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errOSError, errno );
      return XrdCl::XRootDStatus();
    }
    XrdCl::FileSystem fs( target );
    return fs.Rm( target.GetPathWithParams() );
  }
}

namespace XrdCl
{
  XRootDStatus ValidateCopyChecksum( const PropertyList &properties )
  {
    if( !properties.HasProperty( "checkSumVerify" ) ) return XRootDStatus();
    std::string mode = properties.Get<std::string>( "checkSumVerify" );
    if( mode != "source" && mode != "target" && mode != "both" )
      return InvalidChecksum( "checkSumVerify must be source, target or both" );

    URL source( properties.Get<std::string>( "source" ) );
    URL target( properties.Get<std::string>( "target" ) );
    if( !source.IsValid() || !target.IsValid() )
      return InvalidChecksum( "Invalid URL for explicit checksum verification" );
    if( source.GetProtocol() == "stdio" || target.GetProtocol() == "stdio" ||
        source.IsMetalink() || target.IsMetalink() ||
        Enabled( properties, "zipArchive" ) || Enabled( properties, "zipAppend" ) ||
        Enabled( properties, "dynamicSource" ) || Enabled( properties, "recursive" ) ||
        source.GetParams().count( "xrdcl.unzip" ) )
      return InvalidChecksum( "Explicit checksum verification does not support "
                              "stdio, Metalink, ZIP, dynamic or recursive jobs", ENOTSUP );

    std::string type = LowerCase( properties.Get<std::string>( "checkSumType" ) );
    if( !ValidType( type ) ) return InvalidChecksum( "Invalid checksum algorithm" );
    if( type == "auto" && source.IsLocalFile() && target.IsLocalFile() )
      type = "adler32";

    size_t width = ChecksumWidth( type );
    if( type != "auto" && ( ( mode != "target" && source.IsLocalFile() ) ||
                            ( mode != "source" && target.IsLocalFile() ) ) )
    {
      CheckSumManager *manager = DefaultEnv::GetCheckSumManager();
      std::unique_ptr<XrdCksCalc> calculator( manager ? manager->GetCalculator( type ) : 0 );
      if( !calculator )
        return InvalidChecksum( "Checksum algorithm is unavailable for local verification",
                                ENOTSUP );
      int bytes = 0;
      calculator->Type( bytes );
      if( bytes <= 0 || bytes > XrdCksData::ValuSize )
        return InvalidChecksum( "Invalid native checksum length", ENOTSUP );
      width = 2 * bytes;
    }

    std::string preset = properties.Get<std::string>( "checkSumPreset" );
    if( preset.empty() && mode != "both" )
      return InvalidChecksum( "Source or target verification requires an expected checksum" );
    if( !preset.empty() && !NormalizeDigest( type, preset, width ) )
      return InvalidChecksum( "Invalid expected checksum digest" );
    return XRootDStatus();
  }

  XRootDStatus PrepareCopyChecksum( PropertyList &properties,
                                   PropertyList &results )
  {
    if( !properties.HasProperty( "checkSumVerify" ) ) return XRootDStatus();
    properties.Set( "checkSumVerifyPrepared", false );
    XRootDStatus status = ValidateCopyChecksum( properties );
    if( !status.IsOK() ) return status;

    std::string mode = properties.Get<std::string>( "checkSumVerify" );
    URL source( properties.Get<std::string>( "source" ) );
    URL target( properties.Get<std::string>( "target" ) );
    struct stat sourceInfo = {}, targetInfo = {};
    status = RegularFile( source, "source", true, &sourceInfo );
    if( !status.IsOK() ) return status;
    status = RegularFile( target, "target", false, &targetInfo );
    if( !status.IsOK() ) return status;
    if( source.IsLocalFile() && target.IsLocalFile() &&
        targetInfo.st_mode &&
        sourceInfo.st_dev == targetInfo.st_dev && sourceInfo.st_ino == targetInfo.st_ino )
      return InvalidChecksum( "Checksum source and target refer to the same file" );

    std::string type = LowerCase( properties.Get<std::string>( "checkSumType" ) );
    if( !ValidType( type ) ) return InvalidChecksum( "Invalid checksum algorithm" );
    if( type == "auto" )
    {
      // Legacy inference intentionally disables local-to-local checksumming.
      // Explicit verification requests it, so use the native default there.
      type = source.IsLocalFile() && target.IsLocalFile() ? "adler32" :
             Utils::InferChecksumType( source, target );
      if( !ValidType( type ) )
        return XRootDStatus( stError, errCheckSumError, ENOTSUP,
                             "Could not infer checksum type for explicit verification" );
    }

    size_t width = ChecksumWidth( type );
    if( ( mode != "target" && source.IsLocalFile() ) ||
        ( mode != "source" && target.IsLocalFile() ) )
    {
      CheckSumManager *manager = DefaultEnv::GetCheckSumManager();
      std::unique_ptr<XrdCksCalc> calculator( manager ? manager->GetCalculator( type ) : 0 );
      if( !calculator )
        return InvalidChecksum( "Checksum algorithm is unavailable for local verification",
                                ENOTSUP );
      int bytes = 0;
      calculator->Type( bytes );
      if( bytes <= 0 || bytes > XrdCksData::ValuSize )
        return InvalidChecksum( "Invalid native checksum length", ENOTSUP );
      width = 2 * bytes;
    }

    std::string preset = properties.Get<std::string>( "checkSumPreset" );
    if( preset.empty() && mode != "both" )
      return InvalidChecksum( "Source or target verification requires an expected checksum" );
    if( !preset.empty() && !NormalizeDigest( type, preset, width ) )
      return InvalidChecksum( "Invalid expected checksum digest" );
    properties.Set( "checkSumVerifyExpected", preset.empty() ? "" : type + ':' + preset );
    properties.Set( "checkSumType", type );
    properties.Set( "checkSumMode", "none" );

    if( mode != "target" )
    {
      std::string checksum;
      status = Calculate( source, type, checksum );
      if( !status.IsOK() ) return status;
      results.Set( "sourceCheckSum", checksum );
      if( !preset.empty() && checksum != type + ':' + preset )
        return XRootDStatus( stError, errCheckSumError, EDOM,
                             "Source checksum does not match the expected checksum" );
    }
    properties.Set( "checkSumVerifyPrepared", true );
    return XRootDStatus();
  }

  XRootDStatus FinishCopyChecksum( PropertyList       &properties,
                                  PropertyList       &results,
                                  const XRootDStatus &transferStatus )
  {
    if( !transferStatus.IsOK() || !properties.HasProperty( "checkSumVerify" ) )
      return transferStatus;
    if( !Enabled( properties, "checkSumVerifyPrepared" ) )
      return XRootDStatus( stError, errInternal, EINVAL,
                           "Explicit checksum verification was not prepared" );
    if( properties.Get<std::string>( "checkSumVerify" ) == "source" )
      return transferStatus;

    URL target( properties.Get<std::string>( "target" ) );
    struct stat targetInfo = {};
    XRootDStatus status = RegularFile( target, "target", true, &targetInfo );
    if( !status.IsOK() ) return status;
    std::string checksum;
    status = Calculate( target, properties.Get<std::string>( "checkSumType" ), checksum );
    if( !status.IsOK() ) return status;
    results.Set( "targetCheckSum", checksum );
    std::string expected = properties.Get<std::string>( "checkSumVerifyExpected" );
    if( expected.empty() ) expected = results.Get<std::string>( "sourceCheckSum" );
    if( checksum == expected ) return transferStatus;

    std::string message = "Target checksum does not match the expected checksum";
    if( Enabled( properties, "rmOnBadCksum" ) )
    {
      XRootDStatus removal = RemoveTarget( target, targetInfo );
      results.Set( "checkSumRemovalStatus", removal );
      if( !removal.IsOK() ) message += "; target removal failed: " + removal.ToStr();
    }
    return XRootDStatus( stError, errCheckSumError, EDOM, message );
  }
}
