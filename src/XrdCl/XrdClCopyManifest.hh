//------------------------------------------------------------------------------
// Copyright (c) 2026 by European Organization for Nuclear Research (CERN)
// This file is part of XRootD and is distributed under the LGPL.
// Private xrdcp helpers; this header is not part of the installed client API.
//------------------------------------------------------------------------------
#ifndef XRDCL_COPY_MANIFEST_HH
#define XRDCL_COPY_MANIFEST_HH

#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClPropertyList.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdOuc/XrdOucJson.hh"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace XrdCpBatch
{
  struct Input
  {
    std::string source, target, jobTarget, checksumType, checksumPreset;
    std::string checksumMode, checksumVerify;
    bool targetIsDir = false;
    bool usePreparedTarget = false;
  };

  inline std::string Lower( std::string value )
  {
    std::transform( value.begin(), value.end(), value.begin(),
                    [](unsigned char c) { return std::tolower(c); } );
    return value;
  }

  inline bool PlainString( const std::string &value )
  {
    return !value.empty() && std::none_of( value.begin(), value.end(),
      [](unsigned char c) { return c < 32; } );
  }

  inline std::string LexicalPath( const std::string &path )
  {
    std::vector<std::string> parts;
    size_t start = 0;
    while( start <= path.size() )
    {
      size_t end = path.find( '/', start );
      if( end == std::string::npos ) end = path.size();
      std::string part = path.substr( start, end - start );
      if( part == ".." ) { if( !parts.empty() ) parts.pop_back(); }
      else if( !part.empty() && part != "." ) parts.push_back( part );
      start = end + 1;
    }
    std::string result;
    for( const auto &part : parts ) result += '/' + part;
    return result.empty() ? "/" : result;
  }

  inline std::string AbsolutePath( const std::string &path )
  {
    if( !path.empty() && path.front() == '/' ) return path;
    char *cwd = getcwd( nullptr, 0 );
    if( !cwd ) throw std::runtime_error( "Unable to resolve the working directory." );
    std::string result = std::string( cwd ) + '/' + path;
    free( cwd );
    return result;
  }

  inline std::string CanonicalLocalPath( const std::string &path, size_t depth = 0 )
  {
    if( depth >= 40 ) throw std::runtime_error("Too many symbolic links in manifest path.");
    std::string parent = AbsolutePath( path ), tail;
    // Resolve existing ancestors as well as existing files. This detects aliases
    // through symlinked parent directories when the destination does not exist.
    while( true )
    {
      char *resolved = realpath( parent.c_str(), nullptr );
      if( resolved )
      {
        std::string result = LexicalPath( std::string( resolved ) + tail );
        free( resolved );
        return result;
      }
      // realpath fails for dangling symlinks, but creating such a target still
      // follows the link. Resolve it explicitly to detect output aliases.
      struct stat info;
      if( lstat(parent.c_str(), &info) == 0 && S_ISLNK(info.st_mode) )
      {
        std::vector<char> buffer(info.st_size > 0 ? info.st_size + 1 : 4096);
        ssize_t length = readlink(parent.c_str(), buffer.data(), buffer.size());
        if( length < 0 || static_cast<size_t>(length) >= buffer.size() )
          throw std::runtime_error("Unable to resolve a manifest symbolic link.");
        std::string linked(buffer.data(), length);
        if( linked.empty() || linked.front() != '/' )
          linked = parent.substr(0, parent.rfind('/') + 1) + linked;
        return CanonicalLocalPath(linked + tail, depth + 1);
      }
      size_t slash = parent.rfind('/');
      if( slash == std::string::npos || parent == "/" )
        return LexicalPath( AbsolutePath( path ) );
      tail = parent.substr( slash ) + tail;
      parent = slash ? parent.substr( 0, slash ) : "/";
    }
  }

  inline std::string DecodePath( const std::string &path )
  {
    std::string result;
    for( size_t i = 0; i < path.size(); ++i )
    {
      if( path[i] == '%' && i + 2 < path.size() &&
          std::isxdigit( static_cast<unsigned char>(path[i+1]) ) &&
          std::isxdigit( static_cast<unsigned char>(path[i+2]) ) )
      {
        auto nibble = [](unsigned char c) { return c <= '9' ? c-'0' : std::tolower(c)-'a'+10; };
        result += char( nibble(path[i+1])*16 + nibble(path[i+2]) );
        i += 2;
      }
      else result += path[i];
    }
    return result;
  }

  inline std::string SanitizeURL( std::string value )
  {
    size_t scheme = value.find( "://" );
    if( scheme == std::string::npos ) return value;
    size_t end = value.find_first_of( "?#", scheme + 3 );
    if( end != std::string::npos ) value.erase( end );
    size_t slash = value.find( '/', scheme + 3 );
    size_t at = value.rfind( '@', slash == std::string::npos ? value.size() : slash );
    if( at != std::string::npos && at >= scheme + 3 )
      value.erase( scheme + 3, at - scheme - 2 );
    return value;
  }

  inline std::string SanitizeMessage( std::string value,
                                      const std::vector<Input> &inputs )
  {
    // Native errors may repeat the exact submitted URLs. Replace them first,
    // then sanitize any redirected URLs included by the server.
    for( const auto &input : inputs )
      for( const auto &raw : {input.source, input.target} )
      {
        auto clean = SanitizeURL( raw );
        if( raw == clean ) continue;
        size_t pos = 0;
        while( (pos = value.find( raw, pos )) != std::string::npos )
        { value.replace( pos, raw.size(), clean ); pos += clean.size(); }
        XrdCl::URL url( raw );
        std::vector<std::string> secrets;
        if( !url.GetPassword().empty() ) secrets.push_back( url.GetPassword() );
        if( !url.GetUserName().empty() ) secrets.push_back( url.GetUserName() );
        for( const auto &param : url.GetParams() )
          if( !param.second.empty() ) secrets.push_back( param.second );
        for( const auto &secret : secrets )
        {
          pos = 0;
          while( (pos = value.find( secret, pos )) != std::string::npos )
          { value.replace( pos, secret.size(), "[redacted]" ); pos += 10; }
        }
      }
    size_t search = 0;
    while( (search = value.find( "://", search )) != std::string::npos )
    {
      size_t start = search;
      while( start && (std::isalnum(static_cast<unsigned char>(value[start-1])) ||
                       value[start-1] == '+' || value[start-1] == '-' ) ) --start;
      size_t end = value.find_first_of( " \t\r\n\"'<>", search + 3 );
      if( end == std::string::npos ) end = value.size();
      std::string clean = SanitizeURL( value.substr( start, end-start ) );
      value.replace( start, end-start, clean );
      search = start + clean.size();
    }
    for( const auto &label : {"bearer ", "authz=", "access_token=", "password="} )
    {
      search = 0;
      while( (search = Lower(value).find(label, search)) != std::string::npos )
      {
        size_t start = search + std::char_traits<char>::length(label);
        size_t end = value.find_first_of(" \t\r\n\"'&<>", start);
        if( end == std::string::npos ) end = value.size();
        value.replace(start, end-start, "[redacted]");
        search = start + 10;
      }
    }
    return value;
  }

  inline std::string NormalizeOperand( const std::string &operand )
  {
    if( operand == "-" ) throw std::runtime_error( "Manifest jobs cannot use stdin or stdout." );
    bool localPath = operand.find( "://" ) == std::string::npos;
    if( localPath && operand.find('?') != std::string::npos )
      throw std::runtime_error( "Manifest local paths cannot contain '?'." );
    std::string value = localPath ?
                        "file://" + AbsolutePath( operand ) : operand;
    XrdCl::URL url( value );
    if( !url.IsValid() ) throw std::runtime_error( "Invalid manifest URL." );
    const std::string &protocol = url.GetProtocol();
    if( protocol != "file" && protocol != "root" && protocol != "roots" &&
        protocol != "xroot" && protocol != "xroots" &&
        protocol != "http" && protocol != "https" && protocol != "dav" &&
        protocol != "davs" && protocol != "pelican" &&
        protocol != "s3" ) throw std::runtime_error( "Unsupported manifest URL protocol." );
    if( protocol == "file" && !url.IsLocalFile() )
      throw std::runtime_error( "Manifest file URLs must refer to the local host." );
    if( protocol == "file" && operand.find('?') != std::string::npos )
      throw std::runtime_error( "Manifest local file URLs cannot contain '?'." );
    if( url.IsMetalink() || url.GetParams().count("xrdcl.unzip") )
      throw std::runtime_error( "Manifest entries do not support Metalink or ZIP sources." );
    if( protocol == "file" )
    {
      struct stat info;
      if( stat( url.GetPath().c_str(), &info ) == 0 && S_ISDIR(info.st_mode) )
        throw std::runtime_error( "Manifest entries must name individual files, not directories." );
    }
    return value;
  }

  inline std::set<std::string> Identities( const std::string &value )
  {
    XrdCl::URL url( value );
    if( url.IsLocalFile() )
    {
      std::set<std::string> result {"file:" + CanonicalLocalPath(url.GetPath())};
      struct stat info;
      if( stat( url.GetPath().c_str(), &info ) == 0 )
        result.insert( "inode:" + std::to_string(info.st_dev) + ':' + std::to_string(info.st_ino) );
      return result;
    }
    std::string protocol = Lower(url.GetProtocol());
    if( protocol == "roots" || protocol == "xroot" || protocol == "xroots" ) protocol = "root";
    if( protocol == "dav" ) protocol = "http";
    if( protocol == "davs" ) protocol = "https";
    int port = url.GetPort();
    // A credential or CGI difference does not make a separate output file.
    return {protocol + "://" + Lower(url.GetHostName()) + ':' +
            std::to_string(port) + LexicalPath(DecodePath(url.GetPath()))};
  }

  inline std::vector<Input> ReadManifest( const std::string &filename,
                                          const Input &defaults )
  {
    std::ifstream stream( filename );
    if( !stream ) throw std::runtime_error( "Unable to open the jobs manifest." );
    std::vector<Input> inputs;
    std::set<std::string> sources, targets;
    std::string line;
    size_t lineNum = 0;
    while( std::getline( stream, line ) )
    {
      ++lineNum;
      if( line.find_first_not_of(" \t\r") == std::string::npos ) continue;
      try
      {
        std::set<std::string> keys;
        bool duplicate = false;
        auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json &value) {
          if( event == nlohmann::json::parse_event_t::key &&
              !keys.insert(value.get<std::string>()).second ) duplicate = true;
          return true;
        };
        auto object = nlohmann::json::parse( line, callback );
        if( !object.is_object() || duplicate ) throw std::runtime_error("Expected an object with unique keys.");
        for( auto it = object.begin(); it != object.end(); ++it )
          if( (it.key() != "source" && it.key() != "target" && it.key() != "checksum" &&
               it.key() != "checksum_mode") || !it.value().is_string() ||
              !PlainString(it.value().get<std::string>()) )
            throw std::runtime_error("Unknown key or invalid string field.");
        if( !object.contains("source") || !object.contains("target") )
          throw std::runtime_error("Both source and target are required.");
        Input input = defaults;
        input.source = NormalizeOperand( object["source"].get<std::string>() );
        input.target = NormalizeOperand( object["target"].get<std::string>() );
        if( object.contains("checksum") )
        {
          std::string checksum = object["checksum"].get<std::string>();
          size_t colon = checksum.find(':');
          input.checksumType = Lower(checksum.substr(0, colon));
          input.checksumPreset = colon == std::string::npos ? "" : checksum.substr(colon+1);
          if( input.checksumType.empty() ||
              !std::all_of(input.checksumType.begin(), input.checksumType.end(),
                [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }) ||
              (colon != std::string::npos && (input.checksumPreset.empty() ||
               !std::all_of(input.checksumPreset.begin(), input.checksumPreset.end(),
                 [](unsigned char c) { return std::isxdigit(c); }))) )
            throw std::runtime_error("Invalid verifying checksum.");
          input.checksumMode = "none";
          if( input.checksumVerify.empty() ) input.checksumVerify = "both";
        }
        if( object.contains("checksum_mode") )
        {
          input.checksumVerify = object["checksum_mode"].get<std::string>();
          input.checksumMode = "none";
        }
        if( !input.checksumVerify.empty() )
        {
          if( input.checksumVerify != "source" && input.checksumVerify != "target" &&
              input.checksumVerify != "both" ) throw std::runtime_error("Invalid checksum_mode.");
          if( input.checksumType.empty() ||
              (input.checksumVerify != "both" && input.checksumPreset.empty()) )
            throw std::runtime_error("Checksum verification requires an algorithm and, for one side, an expected digest.");
        }
        auto sourceKeys = Identities( input.source ), targetKeys = Identities( input.target );
        for( const auto &key : targetKeys )
          if( !targets.insert(key).second ) throw std::runtime_error("Duplicate or aliased target.");
        sources.insert( sourceKeys.begin(), sourceKeys.end() );
        inputs.push_back( std::move(input) );
      }
      catch( const nlohmann::json::exception & )
      {
        // Parser exceptions contain literal input, possibly bearer credentials.
        throw std::runtime_error( "Invalid manifest job on line " + std::to_string(lineNum) +
                                  "; check fields, checksum, and target aliases." );
      }
      catch( const std::runtime_error &error )
      {
        // Our validation diagnostics contain no submitted path or credentials.
        throw std::runtime_error( "Invalid manifest job on line " + std::to_string(lineNum) +
                                  ": " + error.what() );
      }
    }
    if( stream.bad() ) throw std::runtime_error("Unable to read the jobs manifest.");
    if( inputs.empty() ) throw std::runtime_error("The jobs manifest is empty.");
    for( const auto &key : targets )
      if( sources.count(key) ) throw std::runtime_error("Manifest jobs have a source/target alias or dependency.");
    return inputs;
  }

  inline void PrintResults( const std::vector<Input> &inputs,
                            const std::vector<XrdCl::PropertyList *> &results )
  {
    for( size_t i = 0; i < inputs.size(); ++i )
    {
      XrdCl::XRootDStatus status( XrdCl::stError, XrdCl::errOperationInterrupted, 0,
                                 "Job was not executed." );
      results[i]->Get( "status", status );
      std::string state = status.IsOK() ? "succeeded" : "failed";
      results[i]->Get( "jobState", state );
      uint64_t size = 0;
      bool hasSize = results[i]->Get( "size", size );
      std::string sourceChecksum, targetChecksum;
      std::string target = inputs[i].target;
      if( inputs[i].usePreparedTarget ) results[i]->Get( "target", target );
      results[i]->Get( "sourceCheckSum", sourceChecksum );
      results[i]->Get( "targetCheckSum", targetChecksum );
      // A job's native error refers to its own endpoints. Avoid rescanning the
      // entire manifest for every record in a large batch.
      std::vector<Input> thisJob {inputs[i]};
      nlohmann::json record = {
        {"job", i+1}, {"source", SanitizeURL(inputs[i].source)},
        {"target", SanitizeURL(target)}, {"state", state},
        {"status", {{"status", status.status}, {"code", status.code},
                    {"errNo", status.errNo},
                    {"message", SanitizeMessage(status.GetErrorMessage(), thisJob)}}},
        {"bytes", hasSize ? nlohmann::json(size) : nlohmann::json(nullptr)},
        {"source_checksum", sourceChecksum.empty() ? nlohmann::json(nullptr) : nlohmann::json(sourceChecksum)},
        {"target_checksum", targetChecksum.empty() ? nlohmann::json(nullptr) : nlohmann::json(targetChecksum)}
      };
      std::cout << record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    }
  }
}
#endif
