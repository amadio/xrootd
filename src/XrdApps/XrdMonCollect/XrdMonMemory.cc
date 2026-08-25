/******************************************************************************/
/*                                                                            */
/*                      X r d M o n M e m o r y . c c                         */
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

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

#ifdef __solaris__
#include <procfs.h>
#include <sys/types.h>
#endif

#include "XrdMonMemory.hh"

/******************************************************************************/
/*                    X r d M o n P r o c e s s R s s                         */
/******************************************************************************/

std::size_t XrdMonProcessRss()
{
// Platform dispatch follows XrdSysUtils::ExecName(): every branch may fail
// silently, and the fallthrough returns 0 meaning "no reader here".
//
#if defined(__linux__) || defined(__GNU__) || (defined(__FreeBSD_kernel__) && defined(__GLIBC__))
// /proc/self/statm is "size resident shared text lib data dt", in pages. Read
// with open/read rather than <fstream> because this runs on the exporter
// thread at scrape time and must not allocate. Note that systemd's
// ProtectProc=invisible hides *other* processes, never /proc/self, so the
// hardened unit file does not break this.
//
  {char  buff[256];
   int   fd = open("/proc/self/statm", O_RDONLY);
   if (fd < 0) return 0;
   ssize_t n = read(fd, buff, sizeof(buff)-1);
   close(fd);
   if (n <= 0) return 0;
   buff[n] = 0;

   const char *p = strchr(buff, ' ');           // skip field 1 (size)
   if (!p) return 0;
   unsigned long pages = strtoul(p+1, nullptr, 10);

// The OS page size, which is not XrdSys::PageSize -- that constant is the
// fixed 4096-byte pgread/pgwrite protocol unit and would be wrong by 4x on a
// 16 KiB-page arm64 host.
//
   long psz = sysconf(_SC_PAGESIZE);
   if (psz <= 0) return 0;
   return (std::size_t)pages * (std::size_t)psz;
  }
#elif defined(__APPLE__)
  {mach_task_basic_info_data_t info;
   mach_msg_type_number_t      cnt = MACH_TASK_BASIC_INFO_COUNT;
   if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                 (task_info_t)&info, &cnt) != KERN_SUCCESS) return 0;
   return (std::size_t)info.resident_size;
  }
#elif defined(__solaris__)
  {psinfo_t psi;
   int fd = open("/proc/self/psinfo", O_RDONLY);
   if (fd < 0) return 0;
   ssize_t n = read(fd, &psi, sizeof(psi));
   close(fd);
   if (n != (ssize_t)sizeof(psi)) return 0;
   return (std::size_t)psi.pr_rssize * 1024;    // pr_rssize is in KiB
  }
#else
   return 0;
#endif
}

/******************************************************************************/
/*                   X r d M o n C g r o u p L i m i t                        */
/******************************************************************************/

#if defined(__linux__)
namespace
{
// Read a single-value cgroup file. Returns 0 for absent, unreadable, "max", or
// a v1 "unlimited" sentinel (a number so close to SIZE_MAX that no operator
// typed it).
//
std::size_t readLimitFile(const std::string& path)
{
   char buff[64];
   int  fd = open(path.c_str(), O_RDONLY);
   if (fd < 0) return 0;
   ssize_t n = read(fd, buff, sizeof(buff)-1);
   close(fd);
   if (n <= 0) return 0;
   buff[n] = 0;
   if (!strncmp(buff, "max", 3)) return 0;

   unsigned long long v = strtoull(buff, nullptr, 10);
   if (!v || v >= (1ull << 62)) return 0;
   return (std::size_t)v;
}

// This process's path within a controller's hierarchy, from /proc/self/cgroup.
// For v2 that is the line beginning "0::"; for v1 it is the line whose
// comma-separated controller list contains "memory". Empty when absent.
//
std::string cgroupPath(bool v2)
{
   int fd = open("/proc/self/cgroup", O_RDONLY);
   if (fd < 0) return std::string();
   std::string text;
   char  buff[1024];
   ssize_t n;
   while ((n = read(fd, buff, sizeof(buff))) > 0) text.append(buff, (size_t)n);
   close(fd);

   size_t pos = 0;
   while (pos < text.size())
        {size_t eol  = text.find('\n', pos);
         if (eol == std::string::npos) eol = text.size();
         std::string line = text.substr(pos, eol - pos);
         pos = eol + 1;

         // "<hierarchy-id>:<controllers>:<path>"
         size_t c1 = line.find(':');
         if (c1 == std::string::npos) continue;
         size_t c2 = line.find(':', c1 + 1);
         if (c2 == std::string::npos) continue;
         std::string ctl  = line.substr(c1 + 1, c2 - c1 - 1);
         std::string path = line.substr(c2 + 1);

         if (v2) {if (ctl.empty() && line.compare(0, 2, "0:") == 0) return path;}
            else {size_t at = ctl.find("memory");
                  if (at != std::string::npos
                  && (at == 0 || ctl[at-1] == ',')
                  && (at + 6 == ctl.size() || ctl[at+6] == ',')) return path;
                 }
        }
   return std::string();
}

// Walk from this process's own cgroup up to the hierarchy root, returning the
// smallest limit found. A limit set on an ancestor slice bounds this process
// just as effectively as one on its own leaf, and systemd routinely does both.
//
std::size_t walkUp(const std::string& root, const std::string& rel,
                   const char* file)
{
   std::size_t best = 0;
   std::string cur  = rel;
   for (;;)
       {std::size_t v = readLimitFile(root + cur + "/" + file);
        if (v && (!best || v < best)) best = v;
        if (cur.empty() || cur == "/") break;
        size_t slash = cur.rfind('/');
        if (slash == std::string::npos) break;
        cur.erase(slash);
       }
   return best;
}
}
#endif

std::size_t XrdMonCgroupLimit()
{
#if defined(__linux__)
// Unlike XrdMonProcessRss() this runs once, at start-up, so it may allocate
// and use std::string freely -- it is not on the control tick.
//
// The limit is NOT at the root of the hierarchy: under cgroup v2 a unified root
// has no memory.max at all, and systemd's MemoryMax= lands on the service's own
// scope. Reading /sys/fs/cgroup/memory.max would therefore have found nothing
// on precisely the deployments this warning exists for.
//
   {std::string rel = cgroupPath(true);
    if (!rel.empty())
       {std::size_t v = walkUp("/sys/fs/cgroup", rel, "memory.max");
        if (v) return v;
       }
   }

   {std::string rel = cgroupPath(false);
    std::size_t v = walkUp("/sys/fs/cgroup/memory", rel, "memory.limit_in_bytes");
    if (v) return v;
   }
#endif
   return 0;
}

/******************************************************************************/
/*                  X r d M o n R e l e a s e M e m o r y                     */
/******************************************************************************/

void XrdMonReleaseMemory()
{
#if defined(__GLIBC__)
// Since glibc 2.8 this walks every arena, not just the main one, madvise()ing
// free pages away -- which matters here because the daemon runs 8+ threads and
// therefore several arenas.
//
   malloc_trim(0);
#elif defined(__APPLE__)
   malloc_zone_pressure_relief(malloc_default_zone(), 0);
#endif
// musl defines neither, and its mallocng returns memory eagerly, so the
// control loop there simply shrinks the budget without an explicit trim.
}

/******************************************************************************/
/*                  X r d M o n T u n e A l l o c a t o r                     */
/******************************************************************************/

void XrdMonTuneAllocator()
{
#ifdef __GLIBC__
// Each guard matters: glibc reads these environment settings before main()
// runs, so an unconditional mallopt() would silently override a deliberate
// operator choice.

// Without a cap, glibc creates up to 8 x ncores arenas, whose free lists never
// migrate between threads -- so on a large host the daemon's 8+ threads spread
// their peaks across arenas that are each individually retained. Four rather
// than two because the receiver thread's per-datagram std::string is a hot
// allocation path that would contend.
//
   if (!getenv("MALLOC_ARENA_MAX") && !getenv("GLIBC_TUNABLES"))
      mallopt(M_ARENA_MAX, 4);

// Pinning the mmap threshold disables glibc's *dynamic* threshold, which is
// the usual reason RSS never comes back: after it has seen a few large mmap'd
// blocks freed, glibc raises the threshold as far as 32 MiB, after which the
// multi-MB _bulk and OTLP bodies are served from the heap and stay there.
// Served by mmap instead, free() returns them to the OS immediately.
//
   if (!getenv("MALLOC_MMAP_THRESHOLD_"))
      mallopt(M_MMAP_THRESHOLD, 1 << 20);

// So that a free() at the top of the heap actually sbrk()s down rather than
// waiting for the control loop's next malloc_trim().
//
   if (!getenv("MALLOC_TRIM_THRESHOLD_"))
      mallopt(M_TRIM_THRESHOLD, 4 << 20);
#endif
}
