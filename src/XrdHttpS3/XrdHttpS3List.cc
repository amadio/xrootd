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

#include "XrdHttpS3List.hh"

#include "XrdOuc/XrdOucUtils.hh"

#include <algorithm>
#include <charconv>

namespace XrdHttpS3
{

bool ParseListQuery(std::string_view query, ListRequest &req, std::string &err)
{
  bool listV2 = false;

  while (!query.empty()) {
    auto amp = query.find('&');
    auto item = query.substr(0, amp);
    query = amp == std::string_view::npos ? "" : query.substr(amp + 1);
    if (item.empty()) continue;

    auto eq = item.find('=');
    auto key = XrdOucUtils::UrlDecode(std::string(item.substr(0, eq)));
    auto value = eq == std::string_view::npos
                   ? std::string()
                   : XrdOucUtils::UrlDecode(std::string(item.substr(eq + 1)));

    if (key == "list-type") {
      if (value != "2") {
        err = "Only the list type 2 (ListObjectsV2) is supported";
        return false;
      }
      listV2 = true;
    } else if (key == "prefix") {
      req.prefix = value;
    } else if (key == "delimiter") {
      req.delimiter = value;
    } else if (key == "continuation-token") {
      req.continuationToken = value;
    } else if (key == "start-after") {
      req.startAfter = value;
    } else if (key == "encoding-type") {
      if (value != "url") {
        err = "Invalid encoding type: " + value;
        return false;
      }
      req.urlEncoding = true;
    } else if (key == "max-keys") {
      int n = -1;
      auto end = value.data() + value.size();
      auto res = std::from_chars(value.data(), end, n);
      if (value.empty() || res.ec != std::errc() || res.ptr != end || n < 0) {
        err = "Invalid value of max-keys: " + value;
        return false;
      }
      req.maxKeys = std::min(n, MaxKeys);
    }
    // S3 ignores the parameters which it does not know, and so does this
  }

  if (!listV2) {
    err = "Only the request ListObjectsV2 (list-type=2) is supported";
    return false;
  }
  return true;
}

bool SplitPrefix(const std::string &prefix, std::string &dir,
                 std::string &start)
{
  auto slash = prefix.rfind('/');
  dir = slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
  start = slash == std::string::npos ? prefix : prefix.substr(slash + 1);

  std::string_view rest(dir);
  while (!rest.empty()) {
    auto name = rest.substr(0, rest.find('/'));
    if (name.empty() || name == "." || name == "..") return false;
    rest.remove_prefix(name.size() + 1);
  }
  return true;
}

std::string XmlEscape(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:   out += c;
    }
  }
  return out;
}

std::string IsoTime(time_t t)
{
  struct tm tm;
  char buf[32];
  gmtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
  return buf;
}

namespace
{

// A key of the response: an object, or a common prefix of a directory
struct Key
{
  std::string key;
  bool        isPrefix;
  long long   size;
  time_t      mtime;
};

void AddElement(std::string &xml, const char *name, std::string_view value)
{
  xml += '<';
  xml += name;
  xml += '>';
  xml += XmlEscape(value);
  xml += "</";
  xml += name;
  xml += '>';
}

} // namespace

std::string ListObjectsV2(const ListRequest &req, const std::string &dir,
                          std::vector<Entry> entries, bool dirExists,
                          time_t dirMtime)
{
  const auto start = req.prefix.substr(dir.size());

  std::vector<Key> keys;
  if (dirExists && !req.prefix.empty() && req.prefix.back() == '/')
    keys.push_back({req.prefix, false, 0, dirMtime});

  for (auto &e : entries) {
    if (e.name.compare(0, start.size(), start) != 0) continue;
    if (e.isDir)
      keys.push_back({dir + e.name + "/", true, 0, e.mtime});
    else
      keys.push_back({dir + e.name, false, e.size, e.mtime});
  }

  std::sort(keys.begin(), keys.end(),
            [](const Key &a, const Key &b) { return a.key < b.key; });

  // The keys of a page are after the last key of the page before it. The
  // token is that key, and it takes the place of start-after.
  const auto &after =
    req.continuationToken.empty() ? req.startAfter : req.continuationToken;
  auto first = keys.begin();
  if (!after.empty())
    first = std::upper_bound(keys.begin(), keys.end(), after,
                             [](const std::string &a, const Key &b) {
                               return a < b.key;
                             });

  const auto available = static_cast<size_t>(keys.end() - first);
  const auto count = std::min(available, static_cast<size_t>(req.maxKeys));
  const bool truncated = count > 0 && count < available;

  auto encode = [&req](const std::string &s) {
    return req.urlEncoding ? XrdOucUtils::UrlEncode(s) : s;
  };

  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<ListBucketResult "
                    "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
  AddElement(xml, "Name", req.bucket);
  AddElement(xml, "Prefix", encode(req.prefix));
  if (!req.delimiter.empty())
    AddElement(xml, "Delimiter", encode(req.delimiter));
  AddElement(xml, "MaxKeys", std::to_string(req.maxKeys));
  if (req.urlEncoding) AddElement(xml, "EncodingType", "url");
  AddElement(xml, "KeyCount", std::to_string(count));
  AddElement(xml, "IsTruncated", truncated ? "true" : "false");
  if (!req.continuationToken.empty())
    AddElement(xml, "ContinuationToken", req.continuationToken);
  if (truncated)
    AddElement(xml, "NextContinuationToken", (first + count - 1)->key);
  if (!req.startAfter.empty())
    AddElement(xml, "StartAfter", encode(req.startAfter));

  // S3 sends the objects first, then the common prefixes
  for (auto k = first; k != first + count; ++k) {
    if (k->isPrefix) continue;
    xml += "<Contents>";
    AddElement(xml, "Key", encode(k->key));
    AddElement(xml, "LastModified", IsoTime(k->mtime));
    AddElement(xml, "Size", std::to_string(k->size));
    AddElement(xml, "StorageClass", "STANDARD");
    xml += "</Contents>";
  }
  for (auto k = first; k != first + count; ++k) {
    if (!k->isPrefix) continue;
    xml += "<CommonPrefixes>";
    AddElement(xml, "Prefix", encode(k->key));
    xml += "</CommonPrefixes>";
  }

  xml += "</ListBucketResult>\n";
  return xml;
}

std::string ErrorXml(const std::string &code, const std::string &message,
                     const std::string &resource)
{
  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error>";
  AddElement(xml, "Code", code);
  AddElement(xml, "Message", message);
  AddElement(xml, "Resource", resource);
  xml += "</Error>\n";
  return xml;
}

} // namespace XrdHttpS3
