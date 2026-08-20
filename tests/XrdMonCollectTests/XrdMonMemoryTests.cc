//------------------------------------------------------------------------------
// Unit tests for XrdMonMemory: the process-RSS reader and allocator controls
// that back the collector's --max-memory cap.
//
// These are the only tests in the suite that touch real process memory. The
// control loop that consumes them is tested separately against a scripted RSS
// (see the MemLoop fixture in XrdMonCollectTests.cc), so nothing about eviction
// behaviour depends on what this file measures.
//------------------------------------------------------------------------------

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "XrdApps/XrdMonCollect/XrdMonMemory.hh"

#include <gtest/gtest.h>

namespace
{
constexpr std::size_t kMiB = 1024 * 1024;
}

// A reader that compiles but always returns 0 would disable the memory cap
// wholesale and nothing else would notice, so pin down that this platform has
// one. Skipping rather than passing keeps that honest where it genuinely has
// none (Solaris without procfs, an exotic libc).
//
TEST(XrdMonMemory, ProcessRssIsPlausible)
{
   std::size_t rss = XrdMonProcessRss();
   if (!rss) GTEST_SKIP() << "no RSS reader on this platform";

   EXPECT_GT(rss, 1 * kMiB);            // a linked gtest binary is never smaller
   EXPECT_LT(rss, 64ull * 1024 * kMiB); // 64 GiB: a misread unit, not a process
}

// The test that catches a wrong page size. Reading /proc/self/statm gives
// pages, and multiplying by the 4096-byte XrdSys::PageSize protocol constant
// instead of sysconf(_SC_PAGESIZE) understates RSS by 4x on a 16 KiB-page
// arm64 host -- which would silently make the cap four times looser there.
//
TEST(XrdMonMemory, ProcessRssGrowsWithAllocation)
{
   std::size_t before = XrdMonProcessRss();
   if (!before) GTEST_SKIP() << "no RSS reader on this platform";

// Touch every page: an untouched allocation is mapped but not resident, so it
// would not move RSS at all and the test would fail for the wrong reason.
//
   const std::size_t grow = 64 * kMiB;
   std::vector<char> block(grow);
   std::memset(block.data(), 1, block.size());

   std::size_t after = XrdMonProcessRss();
   EXPECT_GT(after, before + 32 * kMiB) << "RSS did not track a 64 MiB resident "
                                           "allocation (page size units?)";
}

// Both are called on paths that run repeatedly (the control tick) or twice
// across the two daemon modes, so neither may be one-shot or fragile.
//
TEST(XrdMonMemory, ReleaseAndTuneAreRepeatable)
{
   XrdMonTuneAllocator();
   XrdMonTuneAllocator();
   XrdMonReleaseMemory();
   XrdMonReleaseMemory();

   if (!XrdMonProcessRss()) GTEST_SKIP() << "no RSS reader on this platform";
   EXPECT_GT(XrdMonProcessRss(), 0u);   // still readable afterwards
}

// The recycling pipes exist so bodies do not reallocate every batch, so a
// normal-sized body must come back with its capacity intact -- otherwise this
// "fix" would quietly undo the optimisation it is protecting.
//
TEST(XrdMonMemory, RecycleKeepsOrdinaryBodyWarm)
{
   std::string b(64 * 1024, 'x');
   std::size_t was = b.capacity();

   XrdMonRecycleBody(b);

   EXPECT_TRUE(b.empty());
   EXPECT_EQ(b.capacity(), was);        // still warm for the next batch
}

// The case that motivates it: one outsized batch would otherwise pin its peak
// in one of the thirty-two recycled slots for the life of the process.
//
TEST(XrdMonMemory, RecycleReleasesOutlierCapacity)
{
   std::string b(8 * kMiB, 'x');
   ASSERT_GT(b.capacity(), kBodyKeepBytes);

   XrdMonRecycleBody(b);

   EXPECT_TRUE(b.empty());
   EXPECT_LE(b.capacity(), kBodyKeepBytes);
}
