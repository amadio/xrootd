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
#endif
