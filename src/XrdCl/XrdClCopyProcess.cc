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

#include "XrdCl/XrdClCopyProcess.hh"
#include "XrdCl/XrdClClassicCopyJob.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClCopyJob.hh"
#include "XrdCl/XrdClCopyChecksum.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClJobManager.hh"
#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClMonitor.hh"
#include "XrdCl/XrdClRedirectorRegistry.hh"
#include "XrdCl/XrdClTPFallBackCopyJob.hh"
#include "XrdCl/XrdClUtils.hh"
#include "XrdOuc/XrdOucPrivateUtils.hh"
#include "XrdSys/XrdSysPthread.hh"

#include <sys/time.h>
#include <mutex>

namespace
{
  // Admission and completion share a lock: a queued wrapper that has not yet
  // started must observe a failure before opening any destination.
  struct CopyAdmission
  {
    CopyAdmission( bool stop, bool failed = false ):
      stopOnFailure( stop ), failed( failed ) {}

    std::mutex mutex;
    bool       stopOnFailure;
    bool       failed;
  };

  XrdCl::XRootDStatus SkippedStatus()
  {
    return XrdCl::XRootDStatus( XrdCl::stError,
                               XrdCl::errOperationInterrupted,
                               kXR_Cancelled,
                               "Copy job skipped after a batch failure" );
  }

  const XrdCl::PropertyList *CopyConfiguration(
                               const std::vector<XrdCl::PropertyList> &jobs )
  {
    for( auto it = jobs.rbegin(); it != jobs.rend(); ++it )
      if( it->HasProperty( "jobType" ) &&
          it->Get<std::string>( "jobType" ) == "configuration" )
        return &*it;
    return 0;
  }

  XrdCl::XRootDStatus FirstJobFailure(
                             const std::vector<XrdCl::PropertyList*> &results )
  {
    for( auto result : results )
    {
      std::string state;
      result->Get( "jobState", state );
      if( state == "failed" )
        return result->Get<XrdCl::XRootDStatus>( "status" );
    }
    return XrdCl::XRootDStatus();
  }

  void ReportUnstartedJob( XrdCl::CopyProgressHandler *progress,
                           uint32_t                    currentJob,
                           uint32_t                    totalJobs,
                           XrdCl::PropertyList        &properties,
                           XrdCl::PropertyList        *results )
  {
    if( !progress ) return;
    XrdCl::URL source, target;
    properties.Get( "source", source );
    properties.Get( "target", target );
    progress->BeginJob( currentJob, totalJobs, &source, &target );
    progress->EndJob( currentJob, results );
  }

  class QueuedCopyJob: public XrdCl::Job
  {
    public:
      QueuedCopyJob( XrdCl::CopyJob             *job,
                     XrdCl::CopyProgressHandler *progress,
                     uint32_t                    currentJob,
                     uint32_t                    totalJobs,
                     CopyAdmission              *admission,
                     XrdSysSemaphore            *sem = 0 ):
        pJob(job), pProgress(progress), pCurrentJob(currentJob),
        pTotalJobs(totalJobs), pAdmission(admission), pSem(sem),
        pWrtRetryCnt( XrdCl::DefaultRetryWrtAtLBLimit ),
        pRetryCnt( XrdCl::DefaultCpRetry ),
        pRetryPolicy( XrdCl::DefaultCpRetryPolicy )
      {
        XrdCl::DefaultEnv::GetEnv()->GetInt( "RetryWrtAtLBLimit", pWrtRetryCnt );
        XrdCl::DefaultEnv::GetEnv()->GetInt( "CpRetry", pRetryCnt );
        XrdCl::DefaultEnv::GetEnv()->GetString( "CpRetryPolicy", pRetryPolicy );
      }

      //------------------------------------------------------------------------
      //! Run the job
      //------------------------------------------------------------------------
      virtual void Run( void * )
      {
        bool skipped = false;
        {
          std::lock_guard<std::mutex> lock( pAdmission->mutex );
          skipped = pAdmission->stopOnFailure && pAdmission->failed;
          pJob->GetResults()->Set( "jobState", skipped ? "skipped" : "running" );
          if( skipped )
            pJob->GetResults()->Set( "status", SkippedStatus() );
        }
        if( skipped )
        {
          ReportUnstartedJob( pProgress, pCurrentJob, pTotalJobs,
                               *pJob->GetProperties(), pJob->GetResults() );
          if( pSem ) pSem->Post();
          return;
        }

        XrdCl::Monitor     *mon = XrdCl::DefaultEnv::GetMonitor();
        timeval             bTOD;

        //----------------------------------------------------------------------
        // Report beginning of the copy
        //----------------------------------------------------------------------
        if( pProgress )
          pProgress->BeginJob( pCurrentJob, pTotalJobs,
                               &pJob->GetSource(),
                               &pJob->GetTarget() );

        if( mon )
        {
          XrdCl::Monitor::CopyBInfo i;
          i.transfer.origin = &pJob->GetSource();
          i.transfer.target = &pJob->GetTarget();
          mon->Event( XrdCl::Monitor::EvCopyBeg, &i );
        }

        gettimeofday( &bTOD, 0 );

        //----------------------------------------------------------------------
        // Do the copy
        //----------------------------------------------------------------------
        XrdCl::XRootDStatus st = XrdCl::PrepareCopyChecksum(
                                  *pJob->GetProperties(), *pJob->GetResults() );
        const bool prepared = st.IsOK();
        while( prepared )
        {
          st = pJob->Run( pProgress );
          //--------------------------------------------------------------------
          // Retry due to write-recovery
          //--------------------------------------------------------------------
          if( !st.IsOK() && st.code == XrdCl::errRetry && pWrtRetryCnt > 0 )
          {
            std::string url;
            pJob->GetResults()->Get( "LastURL", url );
            XrdCl::URL lastURL( url );
            XrdCl::URL::ParamsMap cgi = lastURL.GetParams();
            auto itr = cgi.find( "tried" );
            if( itr != cgi.end() )
            {
              std::string tried = itr->second;
              if( tried[tried.size() - 1] != ',' ) tried += ',';
              tried += lastURL.GetHostName();
              cgi["tried"] = tried;
            }
            else
              cgi["tried"] = lastURL.GetHostName();

            std::string recoveryRedir;
            pJob->GetResults()->Get( "WrtRecoveryRedir", recoveryRedir );
            XrdCl::URL recRedirURL( recoveryRedir );

            std::string target;
            pJob->GetProperties()->Get( "target", target );
            XrdCl::URL trgURL( target );
            trgURL.SetHostName( recRedirURL.GetHostName() );
            trgURL.SetPort( recRedirURL.GetPort() );
            trgURL.SetProtocol( recRedirURL.GetProtocol() );
            trgURL.SetParams( cgi );
            pJob->GetProperties()->Set( "target", trgURL.GetURL() );
            pJob->Init();

            // we have a new job, let's try again
            --pWrtRetryCnt;
            continue;
          }
          //--------------------------------------------------------------------
          // Copy job retry
          //--------------------------------------------------------------------
          if( !st.IsOK() && pRetryCnt > 0 &&
              ( XrdCl::Status::IsSocketError( st.code ) ||
                st.code == XrdCl::errOperationExpired   ||
                st.code == XrdCl::errThresholdExceeded ) )
          {
            if( pRetryPolicy == "continue" )
            {
              pJob->GetProperties()->Set( "force", false );
              pJob->GetProperties()->Set( "continue", true );
            }
            else
            {
              pJob->GetProperties()->Set( "force", true );
              pJob->GetProperties()->Set( "continue", false );
            }
            --pRetryCnt;
            continue;
          }

          // we only loop in case of retry error
          break;
        }

        if( prepared )
          st = XrdCl::FinishCopyChecksum( *pJob->GetProperties(),
                                           *pJob->GetResults(), st );
        {
          std::lock_guard<std::mutex> lock( pAdmission->mutex );
          pJob->GetResults()->Set( "status", st );
          pJob->GetResults()->Set( "jobState", st.IsOK() ? "succeeded" : "failed" );
          if( !st.IsOK() ) pAdmission->failed = true;
        }

        //----------------------------------------------------------------------
        // Report end of the copy
        //----------------------------------------------------------------------
        if( mon )
        {
          std::vector<std::string> sources;
          pJob->GetResults()->Get( "sources", sources );
          XrdCl::Monitor::CopyEInfo i;
          i.transfer.origin = &pJob->GetSource();
          i.transfer.target = &pJob->GetTarget();
          i.sources         = sources.size();
          i.bTOD            = bTOD;
          gettimeofday( &i.eTOD, 0 );
          i.status          = &st;
          mon->Event( XrdCl::Monitor::EvCopyEnd, &i );
        }

        if( pProgress )
          pProgress->EndJob( pCurrentJob, pJob->GetResults() );

        if( pSem )
          pSem->Post();
      }

    private:
      XrdCl::CopyJob             *pJob;
      XrdCl::CopyProgressHandler *pProgress;
      uint32_t                    pCurrentJob;
      uint32_t                    pTotalJobs;
      CopyAdmission              *pAdmission;
      XrdSysSemaphore            *pSem;
      int                         pWrtRetryCnt;
      int                         pRetryCnt;
      std::string                 pRetryPolicy;
  };
};

namespace XrdCl
{
  struct CopyProcessImpl
  {
    std::vector<PropertyList>   pJobProperties;
    std::vector<PropertyList*>  pJobResults;
    std::vector<CopyJob*>       pJobs;
    std::vector<PropertyList>   pPreparedProperties;
    std::vector<URL>            pRegisteredSources;
  };

  //----------------------------------------------------------------------------
  // Destructor
  //----------------------------------------------------------------------------
  CopyProcess::CopyProcess() : pImpl( new CopyProcessImpl() )
  {
  }

  //----------------------------------------------------------------------------
  // Destructor
  //----------------------------------------------------------------------------
  CopyProcess::~CopyProcess()
  {
    CleanUpJobs();
    delete pImpl;
  }

  //----------------------------------------------------------------------------
  // Add job
  //----------------------------------------------------------------------------
  XRootDStatus CopyProcess::AddJob( const PropertyList &properties,
                                    PropertyList       *results )
  {
    Env *env = DefaultEnv::GetEnv();

    //--------------------------------------------------------------------------
    // Process a configuraion job
    //--------------------------------------------------------------------------
    if( properties.HasProperty( "jobType" ) &&
        properties.Get<std::string>( "jobType" ) == "configuration" )
    {
      if( pImpl->pJobProperties.size() > 0 &&
          pImpl->pJobProperties.rbegin()->HasProperty( "jobType" ) &&
          pImpl->pJobProperties.rbegin()->Get<std::string>( "jobType" ) == "configuration" )
      {
        PropertyList &config = *pImpl->pJobProperties.rbegin();
        PropertyList::PropertyMap::const_iterator it;
        for( it = properties.begin(); it != properties.end(); ++it )
          config.Set( it->first, it->second );
      }
      else
        pImpl->pJobProperties.push_back( properties );
      return XRootDStatus();
    }

    //--------------------------------------------------------------------------
    // Validate properties
    //--------------------------------------------------------------------------
    if( !properties.HasProperty( "source" ) )
      return XRootDStatus( stError, errInvalidArgs, 0, "source not specified" );

    if( !properties.HasProperty( "target" ) )
      return XRootDStatus( stError, errInvalidArgs, 0, "target not specified" );

    pImpl->pJobProperties.push_back( properties );
    PropertyList &p = pImpl->pJobProperties.back();

    const char *bools[] = {"target", "force", "posc", "coerce", "makeDir",
                           "zipArchive", "xcp", "preserveXAttr", "rmOnBadCksum",
                           "continue", "zipAppend", "doServer", 0};
    for( int i = 0; bools[i]; ++i )
      if( !p.HasProperty( bools[i] ) )
        p.Set( bools[i], false );

    if( !p.HasProperty( "thirdParty" ) )
      p.Set( "thirdParty", "none" );

    if( !p.HasProperty( "thirdPartyMode" ) )
      p.Set( "thirdPartyMode", "pull" );

    if( !p.HasProperty( "checkSumMode" ) )
      p.Set( "checkSumMode", "none" );
    else
    {
      if( !p.HasProperty( "checkSumType" ) )
      {
        pImpl->pJobProperties.pop_back();
        return XRootDStatus( stError, errInvalidArgs, 0,
                             "checkSumType not specified" );
      }
      else
      {
        //----------------------------------------------------------------------
        // Checksum type has to be case insensitive
        //----------------------------------------------------------------------
        std::string checkSumType;
        p.Get( "checkSumType", checkSumType );
        std::transform(checkSumType.begin(), checkSumType.end(),
                                                checkSumType.begin(), ::tolower);
        p.Set( "checkSumType", checkSumType );
      }
    }

    if( !p.HasProperty( "parallelChunks" ) )
    {
      int val = DefaultCPParallelChunks;
      env->GetInt( "CPParallelChunks", val );
      p.Set( "parallelChunks", val );
    }

    if( !p.HasProperty( "chunkSize" ) )
    {
      int val = DefaultCPChunkSize;
      env->GetInt( "CPChunkSize", val );
      p.Set( "chunkSize", val );
    }

    if( !p.HasProperty( "xcpBlockSize" ) )
    {
      int val = DefaultXCpBlockSize;
      env->GetInt( "XCpBlockSize", val );
      p.Set( "xcpBlockSize", val );
    }

    if( !p.HasProperty( "initTimeout" ) )
    {
      int val = DefaultCPInitTimeout;
      env->GetInt( "CPInitTimeout", val );
      p.Set( "initTimeout", val );
    }

    if( !p.HasProperty( "tpcTimeout" ) )
    {
      int val = DefaultCPTPCTimeout;
      env->GetInt( "CPTPCTimeout", val );
      p.Set( "tpcTimeout", val );
    }

    if( !p.HasProperty( "cpTimeout" ) )
    {
      int val = DefaultCPTimeout;
      env->GetInt( "CPTimeout", val );
      p.Set( "cpTimeout", val );
    }

    if( !p.HasProperty( "dynamicSource" ) )
      p.Set( "dynamicSource", false );

    if( !p.HasProperty( "xrate" ) )
      p.Set( "xrate", 0 );

    if( !p.HasProperty( "xrateThreshold" ) || p.Get<long long>( "xrateThreshold" ) == 0 )
    {
      int val = DefaultXRateThreshold;
      env->GetInt( "XRateThreshold", val );
      p.Set( "xrateThreshold", val );
    }

    //--------------------------------------------------------------------------
    // Insert the properties
    //--------------------------------------------------------------------------
    Log *log = DefaultEnv::GetLog();
    Utils::LogPropertyList( log, UtilityMsg, "Adding job with properties: %s",
                            p );
    pImpl->pJobResults.push_back( results );
    results->Set( "jobState", "queued" );
    results->Set( "status", XRootDStatus() );
    return XRootDStatus();
  }

  //----------------------------------------------------------------------------
  // Prepare the copy jobs
  //----------------------------------------------------------------------------
  XRootDStatus CopyProcess::Prepare()
  {
    CleanUpJobs();
    Log *log = DefaultEnv::GetLog();
    const PropertyList *config = CopyConfiguration( pImpl->pJobProperties );
    bool continueOnPrepareError = false;
    if( config ) config->Get( "continueOnPrepareError", continueOnPrepareError );

    // Keep an independent, stable property list for every input. Preparing a
    // directory target or a TPC URL must not alter the next Prepare() call.
    pImpl->pPreparedProperties.reserve( pImpl->pJobResults.size() );
    for( const auto &props : pImpl->pJobProperties )
    {
      if( props.HasProperty( "jobType" ) &&
          props.Get<std::string>( "jobType" ) == "configuration" )
        continue;
      pImpl->pPreparedProperties.push_back( props );
    }
    pImpl->pJobs.resize( pImpl->pJobResults.size(), 0 );
    for( auto result : pImpl->pJobResults )
    {
      result->Set( "jobState", "queued" );
      result->Set( "status", XRootDStatus() );
    }

    log->Debug( UtilityMsg, "CopyProcess: %zu jobs to prepare",
                pImpl->pJobs.size() );

    for( size_t i = 0; i < pImpl->pPreparedProperties.size(); ++i )
    {
      PropertyList &props = pImpl->pPreparedProperties[i];
      PropertyList *res = pImpl->pJobResults[i];
      std::string tmp;
      props.Get( "source", tmp );
      URL source = tmp;
      XRootDStatus st;
      if( !source.IsValid() )
      {
        log->Error( UtilityMsg, "Invalid copy source URL: %s",
                    obfuscateAuth( tmp ).c_str() );
        st = XRootDStatus( stError, errInvalidArgs, 0, "invalid source" );
      }

      // A retained Metalink registration is owned by the process even when
      // loading its metadata or later validation of this job fails.
      if( st.IsOK() && source.IsMetalink() )
      {
        RedirectorRegistry &registry = RedirectorRegistry::Instance();
        st = registry.RegisterAndWait( source );
        if( registry.Get( source ) ) pImpl->pRegisteredSources.push_back( source );
      }

      if( st.IsOK() )
      {
        const URL::ParamsMap &cgi = source.GetParams();
        auto itr = cgi.find( "xrdcl.unzip" );
        if( itr != cgi.end() )
        {
          props.Set( "zipArchive", true );
          props.Set( "zipSource", itr->second );
        }
      }

      props.Get( "target", tmp );
      URL target = tmp;
      if( st.IsOK() && !target.IsValid() )
      {
        log->Error( UtilityMsg, "Invalid copy target URL: %s",
                    obfuscateAuth( tmp ).c_str() );
        st = XRootDStatus( stError, errInvalidArgs, 0, "invalid target" );
      }

      if( st.IsOK() && target.GetProtocol() != "stdio" )
      {
        bool targetIsDir = false;
        props.Get( "targetIsDir", targetIsDir );
        if( targetIsDir )
        {
          std::string path = target.GetPath();
          if( path.empty() || path.back() != '/' )
            path += '/';
          std::string fn;
          bool isZip = false;
          props.Get( "zipArchive", isZip );
          if( isZip )
            props.Get( "zipSource", fn );
          else if( source.IsMetalink() )
            fn = RedirectorRegistry::Instance().Get( source )->GetTargetName();
          else
            fn = source.GetPath();

          size_t pos = fn.rfind( '/' );
          if( pos != std::string::npos ) fn = fn.substr( pos + 1 );
          path += fn;
          target.SetPath( path );
          props.Set( "target", target.GetURL() );
          res->Set( "target", target.GetURL() );
        }
      }

      if( st.IsOK() && source.GetProtocol() != "stdio" &&
          source.GetPath().empty() )
        st = XRootDStatus( stError, errInvalidArgs, 0, "no source specified" );
      // A target which is a directory got the name of the source above, thus
      // an empty path here is a target without a file name.
      if( st.IsOK() && target.GetProtocol() != "stdio" &&
          target.GetPath().empty() )
        st = XRootDStatus( stError, errInvalidArgs, 0, "no target specified" );

      if( !st.IsOK() )
      {
        log->Debug( UtilityMsg, "CopyProcess (job #%zu): preparation failed.",
                    i + 1 );
        res->Set( "status", st );
        res->Set( "jobState", "failed" );
        if( continueOnPrepareError ) continue;
        CleanUpJobs();
        return st;
      }

      props.Get( "thirdParty", tmp );
      if( tmp != "none" )
      {
        MarkTPC( props );
        pImpl->pJobs[i] = new TPFallBackCopyJob( i + 1, &props, res );
      }
      else
        pImpl->pJobs[i] = new ClassicCopyJob( i + 1, &props, res );
    }
    return XRootDStatus();
  }

  //----------------------------------------------------------------------------
  // Run the copy jobs
  //----------------------------------------------------------------------------
  XRootDStatus CopyProcess::Run( CopyProgressHandler *progress )
  {
    uint32_t totalJobs = pImpl->pJobs.size();
    for( uint32_t i = 0; i < totalJobs; ++i )
      if( pImpl->pJobs[i] )
      {
        // A repeated Run() starts a new attempt. Only preparation failures,
        // represented by empty job slots, can prevent its initial admission.
        pImpl->pJobResults[i]->Set( "jobState", "queued" );
        pImpl->pJobResults[i]->Set( "status", XRootDStatus() );
      }
    auto failUnstarted = [&]( const XRootDStatus &status )
    {
      for( uint32_t i = 0; i < totalJobs; ++i )
      {
        std::string state;
        pImpl->pJobResults[i]->Get( "jobState", state );
        if( state != "queued" ) continue;
        pImpl->pJobResults[i]->Set( "jobState", "failed" );
        pImpl->pJobResults[i]->Set( "status", status );
        ReportUnstartedJob( progress, i + 1, totalJobs,
                             pImpl->pPreparedProperties[i],
                             pImpl->pJobResults[i] );
      }
      return status;
    };
    const PropertyList *config = CopyConfiguration( pImpl->pJobProperties );
    int parallelThreads = 1;
    bool stopOnFailure = false;
    if( config )
    {
      config->Get( "parallel", parallelThreads );
      config->Get( "stopOnFailure", stopOnFailure );
    }
    if( parallelThreads < 1 || parallelThreads > 255 )
      return failUnstarted( XRootDStatus( stError, errInvalidArgs, 0,
                              "parallel must be between 1 and 255" ) );

    XRootDStatus prepareFailure = FirstJobFailure( pImpl->pJobResults );
    CopyAdmission admission( stopOnFailure, !prepareFailure.IsOK() );
    if( !totalJobs ) return prepareFailure;

    // Failed preparation has no CopyJob, but it still occupies the same input
    // slot and must receive its terminal progress notification.
    for( uint32_t i = 0; i < totalJobs; ++i )
      if( !pImpl->pJobs[i] )
        ReportUnstartedJob( progress, i + 1, totalJobs,
                             pImpl->pPreparedProperties[i],
                             pImpl->pJobResults[i] );

    if( parallelThreads == 1 || (stopOnFailure && !prepareFailure.IsOK()) )
    {
      for( uint32_t i = 0; i < totalJobs; ++i )
      {
        if( !pImpl->pJobs[i] ) continue;
        QueuedCopyJob job( pImpl->pJobs[i], progress, i + 1, totalJobs,
                            &admission );
        job.Run( 0 );
      }
    }
    else
    {
      uint32_t workers = std::min( uint32_t( parallelThreads ), totalJobs );
      JobManager manager( workers );
      manager.Initialize();
      if( !manager.Start() )
      {
        manager.Finalize();
        return failUnstarted( XRootDStatus( stError, errOSError, 0,
                                "Unable to start job manager" ) );
      }

      XrdSysSemaphore completed( 0 );
      std::vector<QueuedCopyJob*> queued;
      uint32_t next = 0;
      uint32_t outstanding = 0;
      while( next < totalJobs || outstanding )
      {
        // Completion publishes a failure under the same lock as admission.
        // Keep only one outstanding wrapper per worker rather than putting the
        // entire batch into JobManager's unbounded queue.
        {
          std::lock_guard<std::mutex> lock( admission.mutex );
          while( next < totalJobs && outstanding < workers &&
                 !(admission.stopOnFailure && admission.failed) )
          {
            uint32_t current = next++;
            if( !pImpl->pJobs[current] ) continue;
            QueuedCopyJob *job = new QueuedCopyJob( pImpl->pJobs[current],
                                  progress, current + 1, totalJobs,
                                  &admission, &completed );
            queued.push_back( job );
            manager.QueueJob( job );
            ++outstanding;
          }
        }
        if( !outstanding ) break;
        completed.Wait();
        --outstanding;
      }

      bool stopped = manager.Stop();
      manager.Finalize();
      for( auto job : queued ) delete job;
      if( !stopped )
        return XRootDStatus( stError, errOSError, 0,
                             "Unable to stop job manager" );

      // Jobs that were never admitted also need a terminal result. Wrappers
      // already admitted perform the same check before starting a transfer.
      for( uint32_t i = next; i < totalJobs; ++i )
      {
        if( !pImpl->pJobs[i] ) continue;
        pImpl->pJobResults[i]->Set( "jobState", "skipped" );
        pImpl->pJobResults[i]->Set( "status", SkippedStatus() );
        ReportUnstartedJob( progress, i + 1, totalJobs,
                             pImpl->pPreparedProperties[i],
                             pImpl->pJobResults[i] );
      }
    }

    // A skipped job can precede the failing job in input order when preparation
    // fails. Return a genuine failure, not the synthesized cancellation status.
    return FirstJobFailure( pImpl->pJobResults );
  }

  void CopyProcess::CleanUpJobs()
  {
    for( auto job : pImpl->pJobs ) delete job;
    pImpl->pJobs.clear();
    pImpl->pPreparedProperties.clear();
    for( const auto &source : pImpl->pRegisteredSources )
      RedirectorRegistry::Instance().Release( source );
    pImpl->pRegisteredSources.clear();
  }
}
