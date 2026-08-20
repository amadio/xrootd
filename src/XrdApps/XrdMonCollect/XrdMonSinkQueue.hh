#ifndef __XRDMONSINKQUEUE_HH__
#define __XRDMONSINKQUEUE_HH__
/******************************************************************************/
/*                                                                            */
/*                    X r d M o n S i n k Q u e u e . h h                     */
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

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

//-----------------------------------------------------------------------------
//! One serialized output body. Shared, and const, because every destination of
//! a given kind receives byte-identical content: a collector feeding a local
//! and a central endpoint serializes once and queues the same buffer twice.
//-----------------------------------------------------------------------------

using XrdMonBody = std::shared_ptr<const std::string>;

//-----------------------------------------------------------------------------
//! A bounded hand-off from the serializer to one sink thread.
//!
//! Unlike XrdMonPipe this never blocks the producer. A destination that is
//! slow, unreachable, or merely rate-limited must not stall the decoder, and
//! with several destinations configured it must not stall the others either --
//! a central collector going down cannot be allowed to take the site's own
//! monitoring with it. When the queue is full the OLDEST body is dropped
//! (fresh monitoring is worth more than stale, the same rule
//! XrdMonDiskCache::dropOldest applies) and the drop is counted.
//!
//! The bound is in bytes as well as in bodies. Bodies range from a few
//! kilobytes to several megabytes, so a count alone bounds nothing useful --
//! and these queues are charged to the process RSS that --max-memory caps.
//!
//! Overflow is deliberately counted apart from the sink's own drop counter: a
//! body dropped after a failed POST says "configure a disk cache", while an
//! overflow says the destination (or its cache) cannot keep up with the input.
//!
//! The producer must not reach past this into the destination's disk cache.
//! That cache belongs to the one sink thread and is unlocked by design.
//-----------------------------------------------------------------------------

class XrdMonSinkQueue
{
public:

//! @param maxBodies  bodies the queue may hold (0 is treated as 1).
//! @param maxBytes   total bytes it may hold (0 = no byte bound).
XrdMonSinkQueue(std::size_t maxBodies, std::size_t maxBytes)
               : capBodies(maxBodies ? maxBodies : 1), capBytes(maxBytes) {}

XrdMonSinkQueue(const XrdMonSinkQueue&)            = delete;
XrdMonSinkQueue& operator=(const XrdMonSinkQueue&) = delete;

//! Producer: queue one body. Never blocks. Drops the oldest bodies until the
//! new one fits; a body larger than the whole budget is still queued, alone,
//! rather than dropped outright (dropping it would lose data no bound can
//! ever recover, and it is transient by construction).
void push(XrdMonBody b)
{
   if (!b) return;
   {std::lock_guard<std::mutex> lk(mtx);
    if (closed) return;
    while (!q.empty() && (q.size() >= capBodies
                          || (capBytes && cur + b->size() > capBytes)))
          {cur -= q.front()->size();
           q.pop_front();
           ++overflow;
          }
    cur += b->size();
    q.push_back(std::move(b));
   }
   data.notify_one();
}

//! Consumer: take the next body, waiting at most `ms` milliseconds. Returns
//! false on timeout and once the queue is closed and drained; tell those
//! apart with closedDrained().
bool takeFor(XrdMonBody& out, int ms)
{
   std::unique_lock<std::mutex> lk(mtx);
   data.wait_for(lk, std::chrono::milliseconds(ms),
                 [&]{return closed || !q.empty();});
   if (q.empty()) return false;
   out = std::move(q.front());
   q.pop_front();
   cur -= out->size();
   return true;
}

//! True once the queue is closed and no bodies remain.
bool closedDrained()
{
   std::lock_guard<std::mutex> lk(mtx);
   return closed && q.empty();
}

//! Unblock the consumer for shutdown. Bodies already queued are still taken.
void close()
{
   {std::lock_guard<std::mutex> lk(mtx); closed = true;}
   data.notify_all();
}

//! Bodies currently waiting (for a gauge).
std::size_t bodies()
{
   std::lock_guard<std::mutex> lk(mtx);
   return q.size();
}

//! Bytes currently waiting (for a gauge).
std::size_t bytes()
{
   std::lock_guard<std::mutex> lk(mtx);
   return cur;
}

//! Bodies dropped because the queue was full (for a counter).
std::uint64_t overflowed()
{
   std::lock_guard<std::mutex> lk(mtx);
   return overflow;
}

private:

std::mutex              mtx;
std::condition_variable data;
std::deque<XrdMonBody>  q;
std::size_t             capBodies;
std::size_t             capBytes;
std::size_t             cur      = 0;
std::uint64_t           overflow = 0;
bool                    closed   = false;
};
#endif
