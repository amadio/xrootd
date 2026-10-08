//------------------------------------------------------------------------------
// Copyright (c) 2011-2014 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
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

#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClUtils.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileStateHandler.hh"
#include "XrdCl/XrdClMessageUtils.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClPlugInInterface.hh"
#include "XrdCl/XrdClPlugInManager.hh"
#include "XrdCl/XrdClDefaultEnv.hh"

#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XProtocol/XProtocol.hh"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace XrdCl
{
  //----------------------------------------------------------------------------
  // The implementation
  //----------------------------------------------------------------------------
  struct FileImpl
  {
    FileImpl( FilePlugIn *plugin ) :
      pStateHandler( std::make_shared<FileStateHandler>( plugin ) )
    {
    }

    FileImpl( bool useVirtRedirector, FilePlugIn *plugin ) :
      pStateHandler( std::make_shared<FileStateHandler>( useVirtRedirector, plugin ) )
    {
    }

    std::shared_ptr<FileStateHandler> pStateHandler;
  };

  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  File::File( bool enablePlugIns ):
    pPlugIn(0),
    pEnablePlugIns( enablePlugIns )
  {
    pImpl = new FileImpl( pPlugIn );
  }

  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  File::File( VirtRedirect virtRedirect, bool enablePlugIns ):
    pPlugIn(0),
    pEnablePlugIns( enablePlugIns )
  {
    pImpl = new FileImpl( virtRedirect == EnableVirtRedirect, pPlugIn );
  }

  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  File::File(const std::string &url, bool enablePlugIns): pPlugIn(0), pEnablePlugIns(enablePlugIns)
  {
    InitPlugin(url);
    pImpl = new FileImpl(pPlugIn);
  }

  //----------------------------------------------------------------------------
  // Destructor
  //----------------------------------------------------------------------------
  File::~File()
  {
    //--------------------------------------------------------------------------
    // This, in principle, should never ever happen. Except for the case
    // when we're interfaced with ROOT that may call this desctructor from
    // its garbage collector, from its __cxa_finalize, ie. after the XrdCl lib
    // has been finalized by the linker. So, if we don't have the log object
    // at this point we just give up the hope.
    // Also, make sure the PostMaster threads are running - if not the Close
    // will hang forever (this could happen when Python interpreter exits).
    //--------------------------------------------------------------------------
    if ( DefaultEnv::GetLog() && DefaultEnv::GetPostMaster()->IsRunning() && IsOpen() )
      XRootDStatus status = Close( nullptr, 0 );
    delete pImpl;
    delete pPlugIn;
  }

  void File::InitPlugin(const std::string &url) {

    if (pEnablePlugIns && !pPlugIn) {
      Log *log = DefaultEnv::GetLog();
      PlugInFactory *fact = DefaultEnv::GetPlugInManager()->GetFactory(url);
      if (fact) {
        pPlugIn = fact->CreateFile(url);
        if (!pPlugIn) {
          log->Error(FileMsg,
                     "Plug-in factory failed to produce a plug-in "
                     "for %s, continuing without one",
                     url.c_str());
        }
      }
    }
  }

  //----------------------------------------------------------------------------
  // Open the file pointed to by the given URL - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Open( const std::string &url,
                           OpenFlags::Flags   flags,
                           Access::Mode       mode,
                           ResponseHandler   *handler,
                           time_t             timeout )
  {
    // Check if we need to install and run a plug-in for this URL
    InitPlugin(url);

    if( (flags & OpenFlags::Dup) || (flags & OpenFlags::Samefs) )
      return XRootDStatus( stError, errInvalidArgs, 0,
             "Dup or Samefs options require a file template to be specified" );

    //--------------------------------------------------------------------------
    // Open the file
    //--------------------------------------------------------------------------
    if( pPlugIn )
      return pPlugIn->Open( url, flags, mode, handler, timeout );

    return FileStateHandler::Open( pImpl->pStateHandler, url, flags, mode, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Open the file pointed to by the given URL - async
  // Alows one to specify template file. Required if using Dup or Samefs flags.
  //----------------------------------------------------------------------------
  XRootDStatus File::OpenUsingTemplate( const File        &rfile,
                                        const std::string &url,
                                        OpenFlags::Flags   flags,
                                        Access::Mode       mode,
                                        ResponseHandler   *handler,
                                        time_t             timeout )
  {
    // Check if we need to install and run a plug-in for this URL
    InitPlugin(url);

    //--------------------------------------------------------------------------
    // Open the file
    //--------------------------------------------------------------------------
    if( pPlugIn )
      return pPlugIn->OpenUsingTemplate( rfile.GetFileTemplate().get(), url,
                            flags, mode, handler, timeout );

    return FileStateHandler::OpenUsingTemplate( pImpl->pStateHandler,
                                   rfile.GetFileTemplate().get(), url, flags,
                                   mode, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Open the file pointed to by the given URL - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Open( const std::string &url,
                           OpenFlags::Flags   flags,
                           Access::Mode       mode,
                           time_t             timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Open( url, flags, mode, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForStatus( &handler );
  }

  //----------------------------------------------------------------------------
  // Open the file pointed to by the given URL - sync
  // Alows one to specify template file. Required if using Dup or Samefs flags.
  //----------------------------------------------------------------------------
  XRootDStatus File::OpenUsingTemplate( const File        &rfile,
                                        const std::string &url,
                                        OpenFlags::Flags   flags,
                                        Access::Mode       mode,
                                        time_t             timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = OpenUsingTemplate( rfile, url, flags, mode, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForStatus( &handler );
  }

  //----------------------------------------------------------------------------
  // Close the file - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Close( ResponseHandler *handler,
                            time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Close( handler, timeout );

    return FileStateHandler::Close( pImpl->pStateHandler, handler, timeout );
  }


  //----------------------------------------------------------------------------
  // Close the file
  //----------------------------------------------------------------------------
  XRootDStatus File::Close( time_t timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Close( &handler, timeout );
    if( !st.IsOK() || st.code == suAlreadyDone )
      return st;

    return MessageUtils::WaitForStatus( &handler );
  }

  //----------------------------------------------------------------------------
  // Obtain status information for this file - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Stat( bool             force,
                           ResponseHandler *handler,
                           time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Stat( force, handler, timeout );

    return FileStateHandler::Stat( pImpl->pStateHandler, force, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Obtain status information for this file - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Stat( bool       force,
                           StatInfo *&response,
                           time_t     timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Stat( force, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForResponse( &handler, response );
  }


  //----------------------------------------------------------------------------
  // Read a data chunk at a given offset - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Read( uint64_t         offset,
                           uint32_t         size,
                           void            *buffer,
                           ResponseHandler *handler,
                           time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Read( offset, size, buffer, handler, timeout );

    return FileStateHandler::Read( pImpl->pStateHandler, offset, size, buffer, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Read a data chunk at a given offset - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Read( uint64_t  offset,
                           uint32_t  size,
                           void     *buffer,
                           uint32_t &bytesRead,
                           time_t    timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Read( offset, size, buffer, &handler, timeout );
    if( !st.IsOK() )
      return st;

    ChunkInfo *chunkInfo = 0;
    XRootDStatus status = MessageUtils::WaitForResponse( &handler, chunkInfo );
    if( status.IsOK() )
    {
      bytesRead = chunkInfo->length;
      delete chunkInfo;
    }
    return status;
  }

  //------------------------------------------------------------------------
  // Read number of pages at a given offset - async
  //------------------------------------------------------------------------
  XRootDStatus File::PgRead( uint64_t         offset,
                             uint32_t         size,
                             void            *buffer,
                             ResponseHandler *handler,
                             time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->PgRead( offset, size, buffer, handler, timeout );

    return FileStateHandler::PgRead( pImpl->pStateHandler, offset, size, buffer, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Read number of pages at a given offset - async
  //------------------------------------------------------------------------
  XRootDStatus File::PgRead( uint64_t               offset,
                             uint32_t               size,
                             void                  *buffer,
                             std::vector<uint32_t> &cksums,
                             uint32_t              &bytesRead,
                             time_t                 timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = PgRead( offset, size, buffer, &handler, timeout );
    if( !st.IsOK() )
      return st;

    PageInfo *pageInfo = 0;
    XRootDStatus status = MessageUtils::WaitForResponse( &handler, pageInfo );
    if( status.IsOK() )
    {
      bytesRead = pageInfo->GetLength();
      cksums = pageInfo->GetCksums();
      delete pageInfo;
    }
    return status;
  }

  //----------------------------------------------------------------------------
  // Write a data chunk at a given offset - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Write( uint64_t         offset,
                            uint32_t         size,
                            const void      *buffer,
                            ResponseHandler *handler,
                            time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Write( offset, size, buffer, handler, timeout );

    return FileStateHandler::Write( pImpl->pStateHandler, offset, size, buffer, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Write a data chunk at a given offset - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Write( uint64_t    offset,
                            uint32_t    size,
                            const void *buffer,
                            time_t      timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Write( offset, size, buffer, &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }


  XRootDStatus File::Write( uint64_t          offset,
                            Buffer          &&buffer,
                            ResponseHandler  *handler,
                            time_t            timeout )
  {
    if( pPlugIn )
      return pPlugIn->Write( offset, std::move( buffer ), handler, timeout );

    return FileStateHandler::Write( pImpl->pStateHandler, offset, std::move( buffer ), handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Write a data chunk at a given offset - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Write( uint64_t    offset,
                            Buffer    &&buffer,
                            time_t      timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Write( offset, std::move( buffer ), &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }

  //------------------------------------------------------------------------
  // Write a data from a given file descriptor at a given offset - async
  //------------------------------------------------------------------------
  XRootDStatus File::Write( uint64_t            offset,
                            uint32_t            size,
                            Optional<uint64_t>  fdoff,
                            int                 fd,
                            ResponseHandler    *handler,
                            time_t              timeout )
  {
    if( pPlugIn )
      return pPlugIn->Write( offset, size, fdoff, fd, handler, timeout );

    return FileStateHandler::Write( pImpl->pStateHandler, offset, size, fdoff, fd, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Write a data from a given file descriptor at a given offset - sync
  //------------------------------------------------------------------------
  XRootDStatus File::Write( uint64_t            offset,
                            uint32_t            size,
                            Optional<uint64_t>  fdoff,
                            int                 fd,
                            time_t              timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Write( offset, size, fdoff, fd, &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }

  //------------------------------------------------------------------------
  // Write number of pages at a given offset - async
  //------------------------------------------------------------------------
  XRootDStatus File::PgWrite( uint64_t               offset,
                              uint32_t               size,
                              const void            *buffer,
                              std::vector<uint32_t> &cksums,
                              ResponseHandler       *handler,
                              time_t                 timeout )
  {
    if( pPlugIn )
      return pPlugIn->PgWrite( offset, size, buffer, cksums, handler, timeout );

    return FileStateHandler::PgWrite( pImpl->pStateHandler, offset, size, buffer, cksums, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Write number of pages at a given offset - sync
  //------------------------------------------------------------------------
  XRootDStatus File::PgWrite( uint64_t               offset,
                              uint32_t               size,
                              const void            *buffer,
                              std::vector<uint32_t> &cksums,
                              time_t                 timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = PgWrite( offset, size, buffer, cksums, &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }

  //----------------------------------------------------------------------------
  // Commit all pending disk writes - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Sync( ResponseHandler *handler,
                           time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Sync( handler, timeout );

    return FileStateHandler::Sync( pImpl->pStateHandler, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Commit all pending disk writes - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Sync( time_t timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Sync( &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }

  //----------------------------------------------------------------------------
  // Truncate the file to a particular size - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Truncate( uint64_t         size,
                               ResponseHandler *handler,
                               time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Truncate( size, handler, timeout );

    return FileStateHandler::Truncate( pImpl->pStateHandler, size, handler, timeout );
  }


  //----------------------------------------------------------------------------
  // Truncate the file to a particular size - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Truncate( uint64_t size, time_t timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Truncate( size, &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }

  //----------------------------------------------------------------------------
  // Preread scattered data tracts in one operation - async
  //----------------------------------------------------------------------------
  XRootDStatus File::PreRead( const TractList &tracts,
                              ResponseHandler *handler,
                              time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->PreRead( tracts, handler, timeout );

//** return FileStateHandler::PreRead( pImpl->pStateHandler, tracts, handler, timeout );
    return XRootDStatus();
  }

  //----------------------------------------------------------------------------
  // Preread scattered data tracts in one operation - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::PreRead( const TractList  &tracts,
                              time_t            timeout )
  {
    SyncResponseHandler handler;
    return PreRead( tracts, &handler, timeout );
  }


  //----------------------------------------------------------------------------
  // Read scattered data chunks in one operation - async
  //----------------------------------------------------------------------------
  XRootDStatus File::VectorRead( const ChunkList &chunks,
                                 void            *buffer,
                                 ResponseHandler *handler,
                                 time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->VectorRead( chunks, buffer, handler, timeout );

    return FileStateHandler::VectorRead( pImpl->pStateHandler, chunks, buffer, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Read scattered data chunks in one operation - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::VectorRead( const ChunkList  &chunks,
                                 void             *buffer,
                                 VectorReadInfo  *&vReadInfo,
                                 time_t            timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = VectorRead( chunks, buffer, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForResponse( &handler, vReadInfo );
  }

  namespace
  {
    using Clock = std::chrono::steady_clock;
    // Protocol defaults apply when server-limit discovery is unavailable.
    struct Limits
    {
      uint32_t count = XrdProto::maxRvecsz;
      uint32_t size = XrdProto::maxRVdsz;
      Clock::time_point expires;
    };
    std::mutex limitsMutex;
    std::map<std::string, Limits> limitsCache;

    class RangeReader;
    class RangeHandler : public ResponseHandler
    {
      public:
        RangeHandler( std::shared_ptr<RangeReader> reader, ChunkList chunks ):
          reader( std::move( reader ) ), chunks( std::move( chunks ) ) {}
        void HandleResponse( XRootDStatus *, AnyObject * ) override;
      private:
        std::shared_ptr<RangeReader> reader;
        ChunkList chunks;
    };

    // Compose existing VectorRead requests; transport, recovery, and plug-in
    // dispatch remain in VectorRead. Parallel pipelines submit every batch at
    // once and may report failure before draining, so use a bounded pump here.
    class RangeReader : public std::enable_shared_from_this<RangeReader>
    {
      public:
        RangeReader( File &file, const ChunkList &chunks,
                     ResponseHandler *handler, uint16_t parallel, time_t timeout ):
          file( file ), chunks( chunks ), handler( handler ), parallel( parallel ),
          timeout( timeout ), deadline( Clock::now() + std::chrono::seconds( timeout ) ) {}

        time_t Remaining() const
        {
          if( !timeout ) return 0;
          auto remaining = deadline - Clock::now();
          if( remaining <= Clock::duration::zero() ) return -1;
          return std::chrono::duration_cast<std::chrono::seconds>( remaining ).count() + 1;
        }

        void Start()
        {
          std::string server;
          if( file.GetProperty( "DataServer", server ) && !server.empty() )
          {
            URL url( server );
            endpoint = url.GetProtocol() + "://" + url.GetHostId() + "/";
            {
              std::lock_guard<std::mutex> lock( limitsMutex );
              auto it = limitsCache.find( endpoint );
              if( it != limitsCache.end() && it->second.expires > Clock::now() )
              {
                limits = it->second;
                endpoint.clear(); // No discovery needed.
              }
            }
          }
          if( endpoint.empty() ) return Pump();
          filesystem.reset( new FileSystem( URL( endpoint ) ) );
          struct QueryHandler : ResponseHandler
          {
            explicit QueryHandler( std::shared_ptr<RangeReader> reader ): reader( reader ) {}
            void HandleResponse( XRootDStatus *status, AnyObject *response ) override
            {
              auto keep = reader;
              keep->Discovered( status, response );
              delete this;
            }
            std::shared_ptr<RangeReader> reader;
          };
          auto query = new QueryHandler( shared_from_this() );
          Buffer arg;
          arg.FromString( "readv_iov_max readv_ior_max" );
          auto status = filesystem->Query( QueryCode::Config, arg, query, Remaining() );
          if( !status.IsOK() ) query->HandleResponse( new XRootDStatus( status ), nullptr );
        }

        void Discovered( XRootDStatus *status, AnyObject *response )
        {
          std::unique_ptr<XRootDStatus> ownStatus( status );
          std::unique_ptr<AnyObject> ownResponse( response );
          Buffer *buffer = nullptr;
          if( response ) response->Get( buffer );
          if( status->IsOK() && buffer )
          {
            std::string text( buffer->GetBuffer(), buffer->GetSize() );
            if( !text.empty() && text.back() == '\0' ) text.pop_back();
            std::istringstream input( text );
            uint64_t count = 0, size = 0;
            std::string extra;
            if( (input >> count >> size) && !(input >> extra) && count && size &&
                count <= XrdProto::maxRvecsz && size <= std::numeric_limits<uint32_t>::max() )
            {
              limits.count = count;
              limits.size = size;
              limits.expires = Clock::now() + std::chrono::seconds( 60 );
              std::lock_guard<std::mutex> lock( limitsMutex );
              if( limitsCache.size() >= 256 ) limitsCache.clear();
              limitsCache[endpoint] = limits;
            }
          }
          Pump();
        }

        void Completed( const XRootDStatus &status )
        {
          {
            std::lock_guard<std::mutex> lock( mutex );
            --active;
            if( result.IsOK() && !status.IsOK() ) result = status;
            if( result.IsOK() && Remaining() < 0 )
              result = XRootDStatus( stError, errOperationExpired );
          }
          Pump();
        }

        void Pump()
        {
          // Submission may complete inline. One pump owns the submission loop;
          // callbacks only update its state until it releases that ownership.
          auto keep = shared_from_this();
          std::unique_lock<std::mutex> lock( mutex );
          if( pumping || finished ) return;
          pumping = true;
          while( result.IsOK() && active < parallel && index < chunks.size() )
          {
            time_t remaining = Remaining();
            if( remaining < 0 )
            {
              result = XRootDStatus( stError, errOperationExpired );
              break;
            }
            ChunkList batch;
            uint64_t bytes = 0;
            while( batch.size() < limits.count && index < chunks.size() )
            {
              const auto &chunk = chunks[index];
              if( !chunk.length ) { ++index; continue; }
              uint32_t length = std::min( chunk.length - offset, limits.size );
              // VectorReadInfo has a 32-bit aggregate size.
              if( bytes + length > std::numeric_limits<uint32_t>::max() ) break;
              batch.emplace_back( chunk.offset + offset, length,
                                  static_cast<char *>( chunk.buffer ) + offset );
              bytes += length;
              offset += length;
              if( offset == chunk.length ) { ++index; offset = 0; }
            }
            if( batch.empty() ) continue;
            ++active;
            lock.unlock();
            auto completion = new RangeHandler( keep, batch );
            auto status = file.VectorRead( batch, nullptr, completion, remaining );
            if( !status.IsOK() )
              completion->HandleResponse( new XRootDStatus( status ), nullptr );
            lock.lock();
          }
          pumping = false;
          if( active || (result.IsOK() && index < chunks.size()) ) return;
          finished = true;
          auto status = new XRootDStatus( result );
          lock.unlock();
          handler->HandleResponse( status, nullptr );
        }

      private:
        File &file;
        ChunkList chunks;
        ResponseHandler *handler;
        uint16_t parallel;
        time_t timeout;
        Clock::time_point deadline;
        Limits limits;
        std::string endpoint;
        std::unique_ptr<FileSystem> filesystem;
        std::mutex mutex;
        size_t index = 0;
        uint32_t offset = 0, active = 0;
        bool pumping = false, finished = false;
        XRootDStatus result;
    };

    void RangeHandler::HandleResponse( XRootDStatus *status, AnyObject *response )
    {
      std::unique_ptr<XRootDStatus> ownStatus( status );
      std::unique_ptr<AnyObject> ownResponse( response );
      if( status->IsOK() && status->code == suContinue ) return;
      VectorReadInfo *info = nullptr;
      if( response ) response->Get( info );
      if( status->IsOK() )
      {
        bool valid = info && info->GetChunks().size() == chunks.size();
        if( valid )
          for( size_t i = 0; i < chunks.size(); ++i )
          {
            const auto &actual = info->GetChunks()[i];
            valid = actual.offset == chunks[i].offset && actual.length == chunks[i].length;
            if( !valid ) break;
          }
        if( !valid ) *status = XRootDStatus( stError, errDataError, 0, "Incomplete range read" );
      }
      reader->Completed( *status );
      delete this;
    }
  }

  //----------------------------------------------------------------------------
  // Read complete ranges with bounded vector requests - async
  //----------------------------------------------------------------------------
  XRootDStatus File::ReadRanges( const ChunkList &chunks, ResponseHandler *handler,
                                uint16_t parallel, time_t timeout )
  {
    if( !handler || !parallel || timeout < 0 ) return XRootDStatus( stError, errInvalidArgs );
    for( const auto &chunk : chunks )
      if( (chunk.length && !chunk.buffer) ||
          chunk.offset > std::numeric_limits<uint64_t>::max() - chunk.length )
        return XRootDStatus( stError, errInvalidArgs );
    if( !IsOpen() ) return XRootDStatus( stError, errInvalidOp );
    std::make_shared<RangeReader>( *this, chunks, handler, parallel, timeout )->Start();
    return XRootDStatus();
  }

  //----------------------------------------------------------------------------
  // Read complete ranges with bounded vector requests - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::ReadRanges( const ChunkList &chunks, uint16_t parallel, time_t timeout )
  {
    SyncResponseHandler handler;
    auto status = ReadRanges( chunks, &handler, parallel, timeout );
    return status.IsOK() ? MessageUtils::WaitForStatus( &handler ) : status;
  }

  //------------------------------------------------------------------------
  // Write scattered data chunks in one operation - async
  //------------------------------------------------------------------------
  XRootDStatus File::VectorWrite( const ChunkList &chunks,
                            ResponseHandler *handler,
                            time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->VectorWrite( chunks, handler, timeout );

    return FileStateHandler::VectorWrite( pImpl->pStateHandler, chunks, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Read scattered data chunks in one operation - sync
  //------------------------------------------------------------------------
  XRootDStatus File::VectorWrite( const ChunkList  &chunks,
                           time_t            timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = VectorWrite( chunks, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForStatus( &handler );
  }

  //------------------------------------------------------------------------
  // Write scattered buffers in one operation - async
  //------------------------------------------------------------------------
  XRootDStatus File::WriteV( uint64_t            offset,
                             const struct iovec *iov,
                             int                 iovcnt,
                             ResponseHandler    *handler,
                             time_t              timeout )
  {
    if( pPlugIn )
      return pPlugIn->WriteV( offset, iov, iovcnt, handler, timeout );

    return FileStateHandler::WriteV( pImpl->pStateHandler, offset, iov, iovcnt, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Write scattered buffers in one operation - sync
  //------------------------------------------------------------------------
  XRootDStatus File::WriteV( uint64_t            offset,
                             const struct iovec *iov,
                             int                 iovcnt,
                             time_t              timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = WriteV( offset, iov, iovcnt, &handler, timeout );
    if( !st.IsOK() )
      return st;

    XRootDStatus status = MessageUtils::WaitForStatus( &handler );
    return status;
  }

  //------------------------------------------------------------------------
  //! Read data into scattered buffers in one operation - async
  //!
  //! @param offset    offset from the beginning of the file
  //! @param iov       list of the buffers to be written
  //! @param iovcnt    number of buffers
  //! @param handler   handler to be notified when the response arrives
  //! @param timeout   timeout value, if 0 then the environment default
  //!                  will be used
  //! @return          status of the operation
  //------------------------------------------------------------------------
  XRootDStatus File::ReadV( uint64_t         offset,
                            struct iovec    *iov,
                            int              iovcnt,
                            ResponseHandler *handler,
                            time_t           timeout )
  {
    return FileStateHandler::ReadV( pImpl->pStateHandler, offset, iov, iovcnt, handler, timeout );
  }

  //------------------------------------------------------------------------
  //! Read data into scattered buffers in one operation - sync
  //!
  //! @param offset    offset from the beginning of the file
  //! @param iov       list of the buffers to be written
  //! @param iovcnt    number of buffers
  //! @param handler   handler to be notified when the response arrives
  //! @param timeout   timeout value, if 0 then the environment default
  //!                  will be used
  //! @return          status of the operation
  //------------------------------------------------------------------------
  XRootDStatus File::ReadV( uint64_t      offset,
                            struct iovec *iov,
                            int           iovcnt,
                            uint32_t     &bytesRead,
                            time_t        timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = ReadV( offset, iov, iovcnt, &handler, timeout );
    if( !st.IsOK() )
      return st;

    VectorReadInfo *vrInfo = 0;
    XRootDStatus status = MessageUtils::WaitForResponse( &handler, vrInfo );
    if( status.IsOK() )
    {
      bytesRead = vrInfo->GetSize();
      delete vrInfo;
    }
    return status;
  }

  //----------------------------------------------------------------------------
  // Performs a custom operation on an open file, server implementation
  // dependent - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Fcntl( const Buffer    &arg,
                            ResponseHandler *handler,
                            time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Fcntl( arg, handler, timeout );

    return FileStateHandler::Fcntl( pImpl->pStateHandler, QueryCode::Code::OpaqueQ, arg, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Performs a custom operation on an open file, server implementation
  // dependent - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Fcntl( QueryCode::Code   queryCode,
                            const Buffer     &arg,
                            Buffer          *&response,
                            time_t            timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Fcntl(queryCode, arg, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForResponse( &handler, response );
  }

  //----------------------------------------------------------------------------
  // Performs a custom operation on an open file, server implementation
  // dependent - async
  //----------------------------------------------------------------------------
  XRootDStatus File::Fcntl( QueryCode::Code  queryCode,
                            const Buffer    &arg,
                            ResponseHandler *handler,
                            time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Fcntl( queryCode, arg, handler, timeout );

    return FileStateHandler::Fcntl(pImpl->pStateHandler, queryCode, arg, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Performs a custom operation on an open file, server implementation
  // dependent - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Fcntl( const Buffer     &arg,
                            Buffer          *&response,
                            time_t            timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Fcntl( arg, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForResponse( &handler, response );
  }

  //------------------------------------------------------------------------
  //! Get access token to a file - async
  //------------------------------------------------------------------------
  XRootDStatus File::Visa( ResponseHandler *handler,
                           time_t           timeout )
  {
    if( pPlugIn )
      return pPlugIn->Visa( handler, timeout );

    return FileStateHandler::Visa( pImpl->pStateHandler, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Get access token to a file - sync
  //----------------------------------------------------------------------------
  XRootDStatus File::Visa( Buffer   *&visa,
                           time_t     timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Visa( &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForResponse( &handler, visa );
  }

  //------------------------------------------------------------------------
  // Set extended attributes - async
  //------------------------------------------------------------------------
  XRootDStatus File::SetXAttr( const std::vector<xattr_t>  &attrs,
                               ResponseHandler             *handler,
                               time_t                       timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::SetXAttr( pImpl->pStateHandler, attrs, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Set extended attributes - sync
  //------------------------------------------------------------------------
  XRootDStatus File::SetXAttr( const std::vector<xattr_t>  &attrs,
                               std::vector<XAttrStatus>    &result,
                               time_t                       timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = SetXAttr( attrs, &handler, timeout );
    if( !st.IsOK() )
      return st;

    std::vector<XAttrStatus> *resp = 0;
    st = MessageUtils::WaitForResponse( &handler, resp );
    if( resp ) result.swap( *resp );
    delete resp;

    return st;
  }

  //------------------------------------------------------------------------
  // Get extended attributes - async
  //------------------------------------------------------------------------
  XRootDStatus File::GetXAttr( const std::vector<std::string>  &attrs,
                               ResponseHandler                 *handler,
                               time_t                           timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::GetXAttr( pImpl->pStateHandler, attrs, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Get extended attributes - sync
  //------------------------------------------------------------------------
  XRootDStatus File::GetXAttr( const std::vector<std::string>  &attrs,
                               std::vector<XAttr>              &result,
                               time_t                           timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = GetXAttr( attrs, &handler, timeout );
    if( !st.IsOK() )
      return st;

    std::vector<XAttr> *resp = 0;
    st = MessageUtils::WaitForResponse( &handler, resp );
    if( resp ) result.swap( *resp );
    delete resp;

    return st;
  }

  //------------------------------------------------------------------------
  // Delete extended attributes - async
  //------------------------------------------------------------------------
  XRootDStatus File::DelXAttr( const std::vector<std::string>  &attrs,
                               ResponseHandler                 *handler,
                               time_t                           timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::DelXAttr( pImpl->pStateHandler, attrs, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Delete extended attributes - sync
  //------------------------------------------------------------------------
  XRootDStatus File::DelXAttr( const std::vector<std::string>  &attrs,
                               std::vector<XAttrStatus>        &result,
                               time_t                           timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = DelXAttr( attrs, &handler, timeout );
    if( !st.IsOK() )
      return st;

    std::vector<XAttrStatus> *resp = 0;
    st = MessageUtils::WaitForResponse( &handler, resp );
    if( resp ) result.swap( *resp );
    delete resp;

    return st;
  }

  //------------------------------------------------------------------------
  // List extended attributes - async
  //------------------------------------------------------------------------
  XRootDStatus File::ListXAttr( ResponseHandler  *handler,
                                time_t            timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::ListXAttr( pImpl->pStateHandler, handler, timeout );
  }

  //------------------------------------------------------------------------
  // List extended attributes - sync
  //------------------------------------------------------------------------
  XRootDStatus File::ListXAttr( std::vector<XAttr>  &result,
                                time_t               timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = ListXAttr( &handler, timeout );
    if( !st.IsOK() )
      return st;

    std::vector<XAttr> *resp = 0;
    st = MessageUtils::WaitForResponse( &handler, resp );
    if( resp ) result.swap( *resp );
    delete resp;

    return st;
  }

  //------------------------------------------------------------------------
  // Create a checkpoint
  //------------------------------------------------------------------------
  XRootDStatus File::Checkpoint( kXR_char                  code,
                                 ResponseHandler          *handler,
                                 time_t                    timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::Checkpoint( pImpl->pStateHandler, code, handler, timeout );
  }

  //------------------------------------------------------------------------
  //! Checkpointed write - async
  //------------------------------------------------------------------------
  XRootDStatus File::ChkptWrt( uint64_t         offset,
                               uint32_t         size,
                               const void      *buffer,
                               ResponseHandler *handler,
                               time_t           timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::ChkptWrt( pImpl->pStateHandler, offset, size, buffer, handler, timeout );
  }

  //------------------------------------------------------------------------
  //! Checkpointed WriteV - async
  //------------------------------------------------------------------------
  XRootDStatus File::ChkptWrtV( uint64_t            offset,
                                const struct iovec *iov,
                                int                 iovcnt,
                                ResponseHandler    *handler,
                                time_t              timeout )
  {
    if( pPlugIn )
      return XRootDStatus( stError, errNotSupported );

    return FileStateHandler::ChkptWrtV( pImpl->pStateHandler, offset, iov, iovcnt, handler, timeout );
  }

  //------------------------------------------------------------------------
  // Try different data server
  //------------------------------------------------------------------------
  XRootDStatus File::TryOtherServer( time_t timeout )
  {
    return FileStateHandler::TryOtherServer( pImpl->pStateHandler, timeout );
  }

  //----------------------------------------------------------------------------
  // Check if the file is open
  //----------------------------------------------------------------------------
  bool File::IsOpen() const
  {
    if( pPlugIn )
      return pPlugIn->IsOpen();

    return pImpl->pStateHandler->IsOpen();
  }

  //------------------------------------------------------------------------
  //! Check if the file is using an encrypted connection
  //------------------------------------------------------------------------
  bool File::IsSecure() const
  {
    if( pPlugIn )
      return false;
    return pImpl->pStateHandler->IsSecure();
  }

  //----------------------------------------------------------------------------
  // Set file property
  //----------------------------------------------------------------------------
  bool File::SetProperty( const std::string &name, const std::string &value )
  {
    if( pPlugIn )
      return pPlugIn->SetProperty( name, value );

    return pImpl->pStateHandler->SetProperty( name, value );
  }

  //----------------------------------------------------------------------------
  // Get file property
  //----------------------------------------------------------------------------
  bool File::GetProperty( const std::string &name, std::string &value ) const
  {
    if( pPlugIn )
      return pPlugIn->GetProperty( name, value );

    return pImpl->pStateHandler->GetProperty( name, value );
  }

  //----------------------------------------------------------------------------
  // Private method: gets the exported file template for use by open or clone
  //----------------------------------------------------------------------------
  std::unique_ptr<ExportedFileTemplate> File::GetFileTemplate() const
  {
    if( pPlugIn )
      return pPlugIn->ExportTemplate();

    return pImpl->pStateHandler->ExportTemplate( pImpl->pStateHandler );
  }

  //----------------------------------------------------------------------------
  // Clone ranges from one or more files
  //----------------------------------------------------------------------------
  XRootDStatus File::Clone( const CloneLocations &locs, ResponseHandler *handler, time_t timeout )
  {
    if( pPlugIn )
      return pPlugIn->Clone( locs, handler, timeout );

    return pImpl->pStateHandler->Clone( pImpl->pStateHandler, locs, handler, timeout );
  }

  //----------------------------------------------------------------------------
  // Clone ranges from one or more files
  //----------------------------------------------------------------------------
  XRootDStatus File::Clone( const CloneLocations &locs, time_t timeout )
  {
    SyncResponseHandler handler;
    XRootDStatus st = Clone( locs, &handler, timeout );
    if( !st.IsOK() )
      return st;

    return MessageUtils::WaitForStatus( &handler );
  }

}
