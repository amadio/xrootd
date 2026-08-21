#ifndef __XRDMONDISKCACHE_HH__
#define __XRDMONDISKCACHE_HH__
/******************************************************************************/
/*                                                                            */
/*                   X r d M o n D i s k C a c h e . h h                      */
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

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

//-----------------------------------------------------------------------------
//! On-failure disk cache for serialized bodies (OpenSearch `_bulk`, OTLP, or
//! shovel frame buffers). When a send fails after retries, the body is written
//! to a file in the cache directory and retried later (oldest first),
//! surviving process restarts. Files are named `<epoch_ms>-<seq><suffix>`
//! (zero-padded so lexical order is chronological) and written via a `.tmp`
//! partial + atomic rename, so a crash never leaves a half-written body to be
//! replayed.
//!
//! Used exclusively by one output thread, so the pending list needs no
//! locking; only the metric accessors are atomic, for the HTTP exporter to
//! read concurrently.
//-----------------------------------------------------------------------------

class XrdMonDiskCache
{
public:

   explicit XrdMonDiskCache(std::string dir, std::string suffix = ".ndjson")
                           : dir(std::move(dir)), suffix(std::move(suffix)) {}

   //! Bound the cache to ~cap bytes (0 = unbounded, the default). When a Store
   //! would exceed the cap, the OLDEST pending bodies are dropped to make room:
   //! fresh monitoring data is worth more than day-old data during a long
   //! outage, and replay latency stays bounded.
   void SetMaxBytes(std::uint64_t cap) { maxBytes = cap; }

   //! Create the directory if needed and seed the pending list from files left
   //! by a previous run (oldest first), removing any stale `.tmp` partials.
   //! Returns false (with err) on a directory error.
   bool Init(std::string& err);

   //! Persist one body. Returns false (with err) on an I/O error.
   bool Store(const std::string& body, std::string& err);

   //! Whether any cached bodies are awaiting replay.
   bool Empty() const { return pending.empty(); }

   //! What the replay callback learned from the sink. Unavailable is the
   //! endpoint being down or overloaded — worth retrying later. Rejected means
   //! the sink understood the body and permanently refused it (e.g. an OTLP
   //! receiver's validation error): replaying it again can only fail the same
   //! way, and because replay is strictly oldest-first, keeping it would block
   //! every body queued behind it for good.
   enum class Verdict : unsigned char { Delivered, Unavailable, Rejected };

   //! Replay the oldest cached body through `cb`. Returns 1 if a body was
   //! replayed and removed (Delivered), quarantined (Rejected — the file is
   //! renamed with a .rejected suffix, kept for post-mortem but out of the
   //! backlog; see err), or dropped because the file was unreadable (see err);
   //! 0 if nothing is pending; and -1 on Unavailable (the file is kept for a
   //! later attempt).
   int ReplayOldest(const std::function<Verdict(const std::string&)>& cb,
                    std::string& err);

   //! Compatibility form for sinks that only distinguish delivered from
   //! unavailable (true/false).
   int ReplayOldest(const std::function<bool(const std::string&)>& cb,
                    std::string& err);

   //! Persist one body the sink has already permanently refused straight into
   //! quarantine (a .rejected file in the cache directory): it is never
   //! replayed, but stays inspectable. Returns false (with err) on an I/O
   //! error.
   bool Quarantine(const std::string& body, std::string& err);

   std::size_t   Files()      const { return files.load(); }
   std::uint64_t Bytes()      const { return bytes.load(); }
   std::uint64_t Stored()     const { return stored.load(); }
   std::uint64_t Replayed()   const { return replayed.load(); }
   std::uint64_t Dropped()    const { return dropped.load(); }
   std::uint64_t Rejected()   const { return rejected.load(); }

private:

   std::string path(const std::string& name) const { return dir + "/" + name; }
   void        dropOldest();
   //! Atomically write `body` to `<name>` (via .tmp + rename); err on failure.
   bool        writeFile(const std::string& name, const std::string& body,
                         std::string& err);
   //! Remove the head of the backlog and account `sz` bytes out of the cache.
   void        popHead(std::uint64_t sz);

   std::string                dir;
   std::string                suffix;        // cache file suffix, e.g. ".ndjson"
   std::deque<std::string>    pending;       // file names, oldest first
   std::uint64_t              seq = 0;        // disambiguates same-millisecond
   std::uint64_t              maxBytes = 0;   // ~cap on cached bytes (0 = off)
   std::atomic<std::size_t>   files{0};
   std::atomic<std::uint64_t> bytes{0};
   std::atomic<std::uint64_t> stored{0};
   std::atomic<std::uint64_t> replayed{0};
   std::atomic<std::uint64_t> dropped{0};     // oldest bodies evicted by the cap
   std::atomic<std::uint64_t> rejected{0};    // bodies quarantined as .rejected
};

#endif
