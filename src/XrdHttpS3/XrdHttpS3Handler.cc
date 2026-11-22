//------------------------------------------------------------------------------
// This file is part of the XRootD framework
//
// Copyright (c) 2026 by European Organization for Nuclear Research (CERN)
//------------------------------------------------------------------------------
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
//------------------------------------------------------------------------------

// An external handler of XrdHttp, which answers the S3 request
// ListObjectsV2 (GET /<bucket>?list-type=2) for a bucket which is the
// directory /<bucket> of the server. The other requests of S3 (GET, PUT,
// HEAD and DELETE of an object) are also requests of HTTP, which XrdHttp
// answers. The handler does not check the SigV4 signature of a request.

#include "XrdHttpS3List.hh"

#include "XrdHttp/XrdHttpExtHandler.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucErrInfo.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdVersion.hh"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace XrdHttpS3;

namespace
{

// The errno of a failure. Some file systems give it with a negative sign.
int Errno(XrdOucErrInfo &einfo)
{
  int rc = einfo.getErrInfo();
  return rc < 0 ? -rc : rc;
}

class S3Handler : public XrdHttpExtHandler
{
public:
  S3Handler(XrdSfsFileSystem *sfs, std::set<std::string> buckets)
    : m_sfs(sfs), m_buckets(std::move(buckets)) {}

  bool MatchesPath(const char *verb, const char *path) override
  {
    return !strcmp(verb, "GET") && !Bucket(path).empty();
  }

  int ProcessReq(XrdHttpExtReq &req) override;

  int Init(const char *) override { return 0; }

private:
  // The bucket of a path /<bucket> or /<bucket>/, or an empty string
  std::string Bucket(std::string_view path) const
  {
    if (path.empty() || path.front() != '/') return {};
    path.remove_prefix(1);
    if (!path.empty() && path.back() == '/') path.remove_suffix(1);
    std::string bucket(path);
    return m_buckets.count(bucket) ? bucket : std::string();
  }

  // Read the entries of a directory. Return 0, or the errno of the failure.
  int ReadDir(const std::string &path, const XrdSecEntity &client,
              std::vector<Entry> &entries);

  static int SendError(XrdHttpExtReq &req, int status, const char *code,
                       const std::string &message);

  XrdSfsFileSystem     *m_sfs;
  std::set<std::string> m_buckets;
};

int S3Handler::SendError(XrdHttpExtReq &req, int status, const char *code,
                         const std::string &message)
{
  auto xml = ErrorXml(code, message, req.resource);
  return req.SendSimpleResp(status, nullptr, "Content-Type: application/xml",
                            xml.c_str(), xml.size());
}

int S3Handler::ReadDir(const std::string &path, const XrdSecEntity &client,
                       std::vector<Entry> &entries)
{
  std::unique_ptr<XrdSfsDirectory> dir(
    m_sfs->newDir(const_cast<char *>(client.tident), 0));
  if (!dir) return ENOMEM;

  if (dir->open(path.c_str(), &client, nullptr) != SFS_OK)
    return Errno(dir->error) ? Errno(dir->error) : EIO;

  // With autoStat(), nextEntry() fills the buffer, else each entry needs a
  // stat of its own
  struct stat st;
  const bool autoStat = dir->autoStat(&st) == SFS_OK;

  int rc = 0;
  while (const char *name = dir->nextEntry()) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    Entry e;
    e.name = name;
    if (!autoStat) {
      XrdOucErrInfo einfo(client.tident);
      auto file = path + "/" + name;
      // An entry which went away after the read of its name is not listed
      if (m_sfs->stat(file.c_str(), &st, einfo, &client) != SFS_OK) continue;
    }
    e.isDir = S_ISDIR(st.st_mode);
    e.size = e.isDir ? 0 : st.st_size;
    e.mtime = st.st_mtime;
    entries.push_back(std::move(e));
  }
  if (Errno(dir->error)) rc = Errno(dir->error);
  dir->close();
  return rc;
}

int S3Handler::ProcessReq(XrdHttpExtReq &req)
{
  ListRequest list;
  list.bucket = Bucket(req.resource);

  // XrdHttp decodes the query before it splits it, thus a value with an
  // encoded '&' or '=' changes. The full resource has the query as the client
  // sent it.
  std::string query;
  auto full = req.headers.find("xrd-http-fullresource");
  if (full != req.headers.end()) {
    auto q = full->second.find('?');
    if (q != std::string::npos) query = full->second.substr(q + 1);
  }

  std::string err;
  if (!ParseListQuery(query, list, err))
    return SendError(req, 400, "InvalidArgument", err);
  if (list.delimiter != "/")
    return SendError(req, 501, "NotImplemented",
                     "Only the delimiter '/' is supported");

  std::string dir, start;
  if (!SplitPrefix(list.prefix, dir, start))
    return SendError(req, 400, "InvalidArgument",
                     "The prefix has an invalid directory: " + list.prefix);

  const auto &client = req.GetSecEntity();
  XrdOucErrInfo einfo(client.tident);
  struct stat st;
  const auto bucketPath = "/" + list.bucket;

  if (m_sfs->stat(bucketPath.c_str(), &st, einfo, &client) != SFS_OK) {
    if (Errno(einfo) == ENOENT)
      return SendError(req, 404, "NoSuchBucket",
                       "The bucket does not exist: " + list.bucket);
    return SendError(req, 403, "AccessDenied", einfo.getErrText());
  }

  auto dirPath = bucketPath + "/" + dir;
  dirPath.pop_back();

  std::vector<Entry> entries;
  bool dirExists = true;
  time_t dirMtime = st.st_mtime;

  if (!dir.empty()) {
    if (m_sfs->stat(dirPath.c_str(), &st, einfo, &client) != SFS_OK ||
        !S_ISDIR(st.st_mode))
      dirExists = false;
    else
      dirMtime = st.st_mtime;
  }

  // S3 has no directories: a prefix without keys has an empty listing
  if (dirExists) {
    int rc = ReadDir(dirPath, client, entries);
    if (rc == EACCES || rc == EPERM)
      return SendError(req, 403, "AccessDenied", strerror(rc));
    if (rc == ENOENT || rc == ENOTDIR)
      dirExists = false;
    else if (rc)
      return SendError(req, 500, "InternalError", strerror(rc));
  }

  auto xml = ListObjectsV2(list, dir, std::move(entries), dirExists, dirMtime);
  return req.SendSimpleResp(200, nullptr, "Content-Type: application/xml",
                            xml.c_str(), xml.size());
}

} // namespace

//------------------------------------------------------------------------------
//! The parameter is a list of the names of the buckets, with commas between
//! them, which S3 does not let a name have. A bucket is a directory of the top
//! directory of the server. For example:
//!
//!   http.exthandler s3 libXrdHttpS3.so bucket1,bucket2
//------------------------------------------------------------------------------

extern "C" XrdHttpExtHandler *XrdHttpGetExtHandler(XrdHttpExtHandlerArgs)
{
  (void)confg;

  auto sfs = static_cast<XrdSfsFileSystem *>(
    myEnv ? myEnv->GetPtr("XrdSfsFileSystem*") : nullptr);
  if (!sfs) {
    eDest->Emsg("S3Initialize", "The S3 handler did not get a file system");
    return nullptr;
  }

  std::set<std::string> buckets;
  std::string_view names(parms ? parms : "");
  while (!names.empty()) {
    std::string bucket(names.substr(0, names.find(',')));
    names.remove_prefix(std::min(names.size(), bucket.size() + 1));
    if (bucket.empty() || bucket.find('/') != std::string::npos ||
        bucket == "." || bucket == "..") {
      eDest->Emsg("S3Initialize", "Invalid name of a bucket:", bucket.c_str());
      return nullptr;
    }
    buckets.insert(bucket);
  }
  if (buckets.empty()) {
    eDest->Emsg("S3Initialize", "The S3 handler requires the names of buckets");
    return nullptr;
  }

  return new S3Handler(sfs, std::move(buckets));
}

XrdVERSIONINFO(XrdHttpGetExtHandler, XrdHttpS3);
