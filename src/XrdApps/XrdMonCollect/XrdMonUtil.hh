#ifndef __XRDMONUTIL_HH__
#define __XRDMONUTIL_HH__
/******************************************************************************/
/*                                                                            */
/*                       X r d M o n U t i l . h h                            */
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
#include <ostream>
#include <string>

#include "XrdOuc/XrdOucJson.hh"

//-----------------------------------------------------------------------------
//! A value written by exactly one thread and read by others.
//!
//! The collector's decode thread owns a pile of counters and occupancy figures
//! that the metrics exporter thread reads at scrape time, through the reader
//! lambdas of XrdMetrics::ObservedFamily -- whose contract is that readers be
//! cheap and thread-safe. Plain integers satisfy neither the language nor
//! ThreadSanitizer there, however benign the torn read would be in practice.
//!
//! Load-modify-store under relaxed ordering rather than fetch_add: with a
//! single writer there is no update to lose, so on any machine with atomic
//! word-sized loads this compiles to exactly the moves the bare integer did,
//! with no lock prefix on the decode path. (A 32-bit target may lower a 64-bit
//! relaxed atomic to a libcall. Correct either way, just not free.)
//!
//! Relaxed is also all that is wanted: these are independent figures read for
//! display, and no consumer needs two of them to agree with each other.
//!
//! Copyable and assignable, unlike std::atomic, so the aggregates holding
//! these stay ordinary values that can be copied and reset wholesale.
//-----------------------------------------------------------------------------

template<class T>
class XrdMonPublished
{
public:

// A user-declared constructor suppresses the implicit default one, and the
// aggregates holding these are reset with `x = Stats{}`.
//
                 XrdMonPublished()                        = default;
                 XrdMonPublished(T n)                     {set(n);}
                 XrdMonPublished(const XrdMonPublished& o) {set(o.get());}

XrdMonPublished& operator=(const XrdMonPublished& o) {set(o.get()); return *this;}
XrdMonPublished& operator=(T n)                      {set(n); return *this;}

                 operator T() const {return get();}
T                get()       const {return v.load(std::memory_order_relaxed);}

XrdMonPublished& operator++()    {set(get() + 1); return *this;}
XrdMonPublished& operator--()    {set(get() - 1); return *this;}
// Returns the old value, not *this: every call site discards it, and this way
// the increment costs no copy.
T                operator++(int) {T old = get(); set(old + 1); return old;}
XrdMonPublished& operator+=(T n) {set(get() + n); return *this;}
XrdMonPublished& operator-=(T n) {set(get() - n); return *this;}

private:

void set(T n) {v.store(n, std::memory_order_relaxed);}

std::atomic<T> v{T()};
};

// Without this googletest prints a failed comparison as a raw byte dump: it
// looks for operator<< before it would ever consider the conversion above.
//
template<class T>
std::ostream& operator<<(std::ostream& os, const XrdMonPublished<T>& p)
{
   return os << p.get();
}

//-----------------------------------------------------------------------------
//! Serialize a document. The only serializer this program uses.
//!
//! nlohmann's default handler throws type_error.316 the moment a string is not
//! valid UTF-8, and every thread in the collector runs its loop bare, so the
//! throw is a process abort -- the receiver, the sinks and everything still
//! unspooled go with it. The bytes in question arrive from the network: LFNs,
//! user descriptors, CGI tails and server error text, none of it under this
//! program's control.
//!
//! So: substitute U+FFFD rather than throw. Losing one field of one record
//! beats losing the collector, and the encoding problem is reported separately
//! (invalid_utf8_total) rather than inferred from a stack trace.
//!
//! Costs nothing over the strict handler: both decode UTF-8 to escape it
//! correctly, and only the action on an invalid sequence differs.
//-----------------------------------------------------------------------------

inline std::string XrdMonDump(const nlohmann::json& j)
{
   return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace XrdMonUtf8
{
//! Length of the well-formed UTF-8 sequence starting at p, or 0 if the bytes
//! there are not one. The ranges are Unicode Table 3-7, which is stricter than
//! "lead byte then continuations": it also rejects overlong encodings, the
//! surrogate range D800-DFFF and anything past U+10FFFF, all of which are
//! sequences nlohmann would still refuse to serialize.
inline int seqLen(const unsigned char* p, std::size_t avail)
{
   const unsigned char c = p[0];
   if (c < 0x80) return 1;

   int len;
   unsigned char lo = 0x80, hi = 0xbf;        // bounds on the *second* byte
   if      (c >= 0xc2 && c <= 0xdf)  len = 2;
   else if (c == 0xe0)              {len = 3; lo = 0xa0;}   // no overlong
   else if (c == 0xed)              {len = 3; hi = 0x9f;}   // no surrogates
   else if (c >= 0xe1 && c <= 0xef)  len = 3;
   else if (c == 0xf0)              {len = 4; lo = 0x90;}   // no overlong
   else if (c == 0xf4)              {len = 4; hi = 0x8f;}   // <= U+10FFFF
   else if (c >= 0xf1 && c <= 0xf3)  len = 4;
   else return 0;                                           // 80-c1, f5-ff

   if (avail < (std::size_t)len || p[1] < lo || p[1] > hi) return 0;
   for (int k = 2; k < len; k++)
       if (p[k] < 0x80 || p[k] > 0xbf) return 0;
   return len;
}
}

//-----------------------------------------------------------------------------
//! Replace every byte that is not part of a well-formed UTF-8 sequence with
//! U+FFFD, in place. @return true if anything was replaced.
//!
//! Call it on a string as it comes off the monitoring wire, before anything is
//! cut out of it: one call then covers every field derived from it, and the
//! state file, the metric labels and both output encodings all see the same
//! clean bytes. Doing it at serialization instead would fix the symptom in one
//! encoder and leave the others.
//!
//! Validates before it rewrites, so the two cases that actually happen -- pure
//! ASCII and correct UTF-8 -- cost one scan and no allocation.
//-----------------------------------------------------------------------------

inline bool XrdMonUtf8Clean(std::string& s)
{
   const unsigned char* b = (const unsigned char*)s.data();
   const std::size_t    n = s.size();

   std::size_t i = 0;
   while (i < n)
        {if (b[i] < 0x80) {i++; continue;}
         int len = XrdMonUtf8::seqLen(b + i, n - i);
         if (!len) break;
         i += len;
        }
   if (i >= n) return false;

   std::string out;
   out.reserve(n);
   out.append(s, 0, i);
   while (i < n)
        {if (b[i] < 0x80) {out += (char)b[i]; i++; continue;}
         int len = XrdMonUtf8::seqLen(b + i, n - i);
         if (len) {out.append(s, i, (std::size_t)len); i += (std::size_t)len;}
            // One U+FFFD per offending byte rather than per run: a truncated
            // sequence and a run of garbage then look different downstream.
            else  {out += "\xef\xbf\xbd"; i++;}
        }
   s.swap(out);
   return true;
}
#endif
