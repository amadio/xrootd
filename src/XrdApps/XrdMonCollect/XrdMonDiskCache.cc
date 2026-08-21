/******************************************************************************/
/*                                                                            */
/*                   X r d M o n D i s k C a c h e . c c                      */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with XRootD in a file called COPYING.LESSER (LGPL license) and file  */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/******************************************************************************/

#include "XrdApps/XrdMonCollect/XrdMonDiskCache.hh"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
bool endsWith(const std::string& s, const std::string& suffix)
{
   return s.size() >= suffix.size() &&
          s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}

bool XrdMonDiskCache::Init(std::string& err)
{
   if (mkdir(dir.c_str(), 0750) != 0 && errno != EEXIST)
      {err = "cannot create cache dir '" + dir + "': " + std::strerror(errno);
       return false;}

   DIR* d = opendir(dir.c_str());
   if (!d)
      {err = "cannot open cache dir '" + dir + "': " + std::strerror(errno);
       return false;}

// Collect completed cache files (oldest first by name); remove stale partials.
//
   std::vector<std::string> names;
   for (struct dirent* e; (e = readdir(d)); )
      {std::string n = e->d_name;
       if (endsWith(n, ".tmp"))  {unlink(path(n).c_str()); continue;}
       if (endsWith(n, suffix))  names.push_back(n);
      }
   closedir(d);
   std::sort(names.begin(), names.end());

// Regular files only. Caches for different destinations are nested under one
// --cache-dir, so a sibling cache's directory can sit inside this one's scan;
// adopting it would have the replay read a directory, fail, and unlink it.
//
   std::uint64_t b = 0;
   for (auto& n : names)
      {struct stat st;
       if (stat(path(n).c_str(), &st) == 0 && S_ISREG(st.st_mode))
          {pending.push_back(n); b += (std::uint64_t)st.st_size;}
      }
   files = pending.size();
   bytes = b;
   return true;
}

// Evict the oldest pending body to make room under the byte cap.
//
void XrdMonDiskCache::dropOldest()
{
   const std::string p = path(pending.front());
   struct stat st;
   std::uint64_t sz = (stat(p.c_str(), &st) == 0) ? (std::uint64_t)st.st_size : 0;
   unlink(p.c_str());
   pending.pop_front();
   files = pending.size();
   if (bytes >= sz) bytes -= sz;
   ++dropped;
}

// Atomically materialize `body` as `<dir>/<name>` via a .tmp partial + rename.
//
bool XrdMonDiskCache::writeFile(const std::string& name,
                                const std::string& body, std::string& err)
{
   std::string tmp = path(name) + ".tmp";
   {std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {err = "cannot create '" + tmp + "'"; return false;}
    f.write(body.data(), (std::streamsize)body.size());
    f.flush();
    if (!f) {err = "cannot write '" + tmp + "'";
             f.close(); unlink(tmp.c_str()); return false;}
   }
   if (rename(tmp.c_str(), path(name).c_str()) != 0)
      {err = std::string("cannot rename cache file: ") + std::strerror(errno);
       unlink(tmp.c_str()); return false;}
   return true;
}

// Take the head of the backlog out of the pending list and byte accounting
// (the caller has already disposed of the file itself).
//
void XrdMonDiskCache::popHead(std::uint64_t sz)
{
   pending.pop_front();
   files = pending.size();
   if (bytes >= sz) bytes -= sz;
}

bool XrdMonDiskCache::Store(const std::string& body, std::string& err)
{
   while (maxBytes && !pending.empty() && bytes + body.size() > maxBytes)
      dropOldest();

   using namespace std::chrono;
   std::uint64_t ms = (std::uint64_t)duration_cast<milliseconds>(
                         system_clock::now().time_since_epoch()).count();
   char name[64];
   std::snprintf(name, sizeof(name), "%013llu-%06llu%s",
                 (unsigned long long)ms, (unsigned long long)seq++,
                 suffix.c_str());

   if (!writeFile(name, body, err)) return false;

   pending.push_back(name);
   files = pending.size();
   bytes += body.size();
   ++stored;
   return true;
}

bool XrdMonDiskCache::Quarantine(const std::string& body, std::string& err)
{
   using namespace std::chrono;
   std::uint64_t ms = (std::uint64_t)duration_cast<milliseconds>(
                         system_clock::now().time_since_epoch()).count();
   char name[80];
   std::snprintf(name, sizeof(name), "%013llu-%06llu%s.rejected",
                 (unsigned long long)ms, (unsigned long long)seq++,
                 suffix.c_str());

   // Not entered into the pending list or the byte budget: a .rejected file is
   // evidence, not backlog. Init() ignores it on restart (wrong suffix).
   if (!writeFile(name, body, err)) return false;
   ++rejected;
   err = "quarantined rejected body as '" + path(name) + "'";
   return true;
}

int XrdMonDiskCache::ReplayOldest(
        const std::function<Verdict(const std::string&)>& cb, std::string& err)
{
   if (pending.empty()) return 0;
   const std::string name = pending.front();
   const std::string p    = path(name);

   std::ifstream f(p, std::ios::binary);
   if (!f)
      {// File vanished or is unreadable: drop it from the backlog.
       err = "cannot read cache file '" + p + "'; dropping";
       struct stat st;
       std::uint64_t sz = (stat(p.c_str(), &st) == 0) ? (std::uint64_t)st.st_size : 0;
       unlink(p.c_str());
       popHead(sz);
       return 1;
      }

   std::stringstream ss;
   ss << f.rdbuf();
   std::string body = ss.str();
   f.close();

   switch (cb(body))
      {case Verdict::Unavailable:
            return -1;                   // sink still down: keep the file

       case Verdict::Rejected:
            // The sink refused this body for good. Move it aside so the bodies
            // behind it can drain, but keep it: what the sink refused and why
            // is exactly what a post-mortem needs.
            if (rename(p.c_str(), (p + ".rejected").c_str()) == 0)
                 err = "quarantined rejected body as '" + p + ".rejected'";
            else {err = std::string("cannot quarantine '") + p + "': "
                      + std::strerror(errno) + "; dropping";
                  unlink(p.c_str());}
            popHead(body.size());
            ++rejected;
            return 1;

       case Verdict::Delivered:
            break;
      }

   unlink(p.c_str());
   popHead(body.size());
   ++replayed;
   return 1;
}

int XrdMonDiskCache::ReplayOldest(
        const std::function<bool(const std::string&)>& cb, std::string& err)
{
   return ReplayOldest(std::function<Verdict(const std::string&)>(
             [&cb](const std::string& b)
             {return cb(b) ? Verdict::Delivered : Verdict::Unavailable;}), err);
}
