/***************************************************************
 *
 * Copyright (C) 2025, Pelican Project, Morgridge Institute for Research
 *
 ***************************************************************/

#ifndef XRDCLS3_S3DOWNLOADHANDLER_HH
#define XRDCLS3_S3DOWNLOADHANDLER_HH

#include "XrdClS3Filesystem.hh"

#include <XrdCl/XrdClXRootDResponses.hh>

namespace XrdClS3 {

XrdCl::XRootDStatus DownloadUrl(const std::string &url, XrdClCurl::HeaderCallout *header_callout, XrdCl::ResponseHandler *handler, Filesystem::timeout_t timeout);

} // namespace XrdClS3
#endif // XRDCLS3_S3DOWNLOADHANDLER_HH