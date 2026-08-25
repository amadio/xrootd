#ifndef __XRDMONMEMORY_HH__
#define __XRDMONMEMORY_HH__
/******************************************************************************/
/*                                                                            */
/*                      X r d M o n M e m o r y . h h                         */
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

#include <cstddef>
#include <string>

//-----------------------------------------------------------------------------
//! Process-level memory introspection and allocator control for xrdmoncollect.
//!
//! Kept out of XrdAppUtils on purpose: this is the only code in the daemon that
//! needs /proc, <mach/mach.h> or <malloc.h>, and XrdAppUtils is an installed
//! shared library. XrdMonDecode reaches these through injected hooks
//! (XrdMonDecode::MemoryHooks) rather than by linking against them, which also
//! lets the unit tests drive the memory control loop with a scripted RSS.
//-----------------------------------------------------------------------------

//! Resident set size of this process, in bytes.
//!
//! Cheap enough (a few microseconds, no allocation) to call from the metrics
//! exporter at scrape time and from the serializer once per control tick.
//!
//! @return the RSS in bytes, or 0 if this platform has no reader. A caller
//!         must treat 0 as "unknown" and hold station, never as "no memory".
std::size_t XrdMonProcessRss();

//! Ask the allocator to return free heap to the operating system.
//!
//! Needed because evicting correlation state does not by itself lower RSS:
//! under glibc the freed chunks stay in the arena's free lists. Without this
//! the RSS control loop cannot converge. A no-op where the allocator has no
//! equivalent (musl's mallocng already releases eagerly).
void XrdMonReleaseMemory();

//! The cgroup memory limit applying to this process, in bytes.
//!
//! Needed because --max-memory is best-effort and a cgroup limit is not: the
//! control loop steers RSS into a band just under the cap, so a limit set at or
//! below --max-memory turns the daemon's normal operating point into an OOM
//! kill. Reading it at start-up is how that gets said before it happens rather
//! than after.
//!
//! @return the limit in bytes, or 0 when there is none, it is "max", or this
//!         platform has no cgroups. Never an error: a caller can only warn.
std::size_t XrdMonCgroupLimit();

//! Pin the glibc allocator parameters that otherwise let RSS ratchet upward.
//!
//! Must be called before any thread starts. Environment settings the operator
//! made are honoured: glibc consumes those before main() runs, so calling
//! mallopt() unconditionally would silently override them.
void XrdMonTuneAllocator();

//! Bytes of capacity a recycled body buffer may keep. Comfortably above a
//! 500-packet OpenSearch `_bulk` body, so steady state never reallocates.
constexpr std::size_t kBodyKeepBytes = 1u << 20;

//! Empty a body buffer for recycling, releasing its capacity if it has grown
//! past kBodyKeepBytes.
//!
//! std::string::clear() keeps the capacity, which is the whole point of
//! recycling: the sixteen bodies in each output pipe cycle round without
//! reallocating. It also means a single outsized batch pins its peak in one of
//! those slots for the life of the process, and there are thirty-two such
//! slots. Keep the common case warm; give the outliers back.
inline void XrdMonRecycleBody(std::string& b)
{
   if (b.capacity() > kBodyKeepBytes) std::string().swap(b);
      else                            b.clear();
}

#endif
