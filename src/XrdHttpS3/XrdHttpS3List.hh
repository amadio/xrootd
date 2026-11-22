#ifndef XRDHTTPS3_LIST_HH
#define XRDHTTPS3_LIST_HH
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

#include <ctime>
#include <string>
#include <string_view>
#include <vector>

// The S3 request ListObjectsV2 for a bucket which is a directory of the
// server. These functions do not access the file system, thus the unit tests
// can test them.

namespace XrdHttpS3
{

//! An entry of a directory of the bucket
struct Entry
{
  std::string name;
  bool        isDir = false;
  long long   size  = 0;
  time_t      mtime = 0;
};

//! The parameters of a request ListObjectsV2
struct ListRequest
{
  std::string bucket;
  std::string prefix;
  std::string delimiter;
  std::string continuationToken;
  std::string startAfter;
  int         maxKeys     = 1000;
  bool        urlEncoding = false;
};

//! The largest number of keys of a response, as S3 has
constexpr int MaxKeys = 1000;

//------------------------------------------------------------------------------
//! Parse the query of a request ListObjectsV2.
//!
//! @param query  the query, with the percent-encoded values
//! @param req    the request, which gets the parameters
//! @param err    the reason of a failure
//!
//! @return true for a valid query with list-type=2, else false
//------------------------------------------------------------------------------

bool ParseListQuery(std::string_view query, ListRequest &req, std::string &err);

//------------------------------------------------------------------------------
//! Split a prefix at its last '/': the directory, which is empty or ends with
//! '/', and the start of the names of the entries.
//!
//! @return false when the directory has an empty name, "." or "..", which no
//!         directory of a file system has
//------------------------------------------------------------------------------

bool SplitPrefix(const std::string &prefix, std::string &dir,
                 std::string &start);

//------------------------------------------------------------------------------
//! Make the XML of the response to a request ListObjectsV2 with the
//! delimiter '/'.
//!
//! @param req       the request
//! @param dir       the directory of the prefix (see SplitPrefix())
//! @param entries   the entries of the directory
//! @param dirExists true when the directory exists. For a prefix which ends
//!                  with '/', the response then has the key of the prefix, as
//!                  S3 has for the object which marks a directory. Thus an
//!                  empty directory exists for an S3 client.
//! @param dirMtime  the modification time of the directory
//------------------------------------------------------------------------------

std::string ListObjectsV2(const ListRequest &req, const std::string &dir,
                          std::vector<Entry> entries, bool dirExists,
                          time_t dirMtime);

//------------------------------------------------------------------------------
//! Make the XML of an S3 error.
//------------------------------------------------------------------------------

std::string ErrorXml(const std::string &code, const std::string &message,
                     const std::string &resource);

//! Escape the characters of a text which XML gives a meaning to
std::string XmlEscape(std::string_view text);

//! Format a time as S3 does, for example "2023-08-21T11:02:53.000Z"
std::string IsoTime(time_t t);

} // namespace XrdHttpS3

#endif
