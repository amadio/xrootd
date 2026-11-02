//------------------------------------------------------------------------------
// Unit tests for XrdMonMemory: the process-RSS reader and allocator controls
// that back the collector's --max-memory cap.
//
// These are the only tests in the suite that touch real process memory. The
// control loop that consumes them is tested separately against a scripted RSS
// (see the MemLoop fixture in XrdMonDecode.cc), so nothing about eviction
// behaviour depends on what this file measures.
//------------------------------------------------------------------------------

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
TEST(Memory, ProcessRssIsPlausible)
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
TEST(Memory, ProcessRssGrowsWithAllocation)
{
   std::size_t before = XrdMonProcessRss();
   if (!before) GTEST_SKIP() << "no RSS reader on this platform";

// Touch every page: an untouched allocation is mapped but not resident, so it
// would not move RSS at all and the test would fail for the wrong reason. The
// stores are volatile because the buffer is never read: without that, the
// compiler may remove the allocation (clang does at -Og, gcc at -O1). A stride
// of 4 KiB touches every page for any page size of 4 KiB or more.
//
   const std::size_t grow = 64 * kMiB;
   std::vector<char> block(grow);
   volatile char *p = block.data();
   for (std::size_t i = 0; i < grow; i += 4096) p[i] = 1;

   std::size_t after = XrdMonProcessRss();
   EXPECT_GT(after, before + 32 * kMiB) << "RSS did not track a 64 MiB resident "
                                           "allocation (page size units?)";
}

// The reader feeds a start-up warning, so its failure mode is a warning that
// never fires (or fires against nonsense) on a containerised host -- silent
// either way. A test machine has no known limit, thus the tests give the
// reader a cgroup tree in a temporary directory: the file of /proc/self/cgroup
// and the files of /sys/fs/cgroup.
//
class CgroupLimitTest : public ::testing::Test
{
protected:
   void SetUp() override
   {
      std::string tmpl = (std::filesystem::temp_directory_path()
                          / "xrdmon-cgroup-XXXXXX").string();
      ASSERT_NE(mkdtemp(tmpl.data()), nullptr);
      dir = tmpl;
   }

   void TearDown() override { std::filesystem::remove_all(dir); }

   // Write the file at the path relative to the temporary directory
   void Write(const std::string& path, const std::string& text)
   {
      std::filesystem::create_directories((dir / path).parent_path());
      std::ofstream(dir / path) << text;
   }

   std::size_t Limit()
   {
      return XrdMonCgroupLimit((dir / "self.cgroup").string(),
                               (dir / "cgroup").string());
   }

   std::filesystem::path dir;
};

TEST_F(CgroupLimitTest, ReadsTheV2LimitOfTheOwnCgroup)
{
   Write("self.cgroup", "0::/system.slice/xrdmon.service\n");
   Write("cgroup/system.slice/xrdmon.service/memory.max", "1073741824\n");
   EXPECT_EQ(Limit(), 1024 * kMiB);
}

TEST_F(CgroupLimitTest, TakesTheSmallestLimitOfTheAncestors)
{
   Write("self.cgroup", "0::/system.slice/xrdmon.service\n");
   Write("cgroup/system.slice/xrdmon.service/memory.max", "2147483648\n");
   Write("cgroup/system.slice/memory.max", "1073741824\n");
   EXPECT_EQ(Limit(), 1024 * kMiB);
}

TEST_F(CgroupLimitTest, TreatsMaxAsNoLimit)
{
   Write("self.cgroup", "0::/system.slice/xrdmon.service\n");
   Write("cgroup/system.slice/xrdmon.service/memory.max", "max\n");
   EXPECT_EQ(Limit(), 0u);
}

TEST_F(CgroupLimitTest, ReadsTheV1LimitOfTheMemoryController)
{
   Write("self.cgroup", "4:cpu,cpuacct:/docker/abc\n3:memory:/docker/abc\n");
   Write("cgroup/memory/docker/abc/memory.limit_in_bytes", "536870912\n");
   EXPECT_EQ(Limit(), 512 * kMiB);
}

TEST_F(CgroupLimitTest, TreatsTheV1UnlimitedValueAsNoLimit)
{
   Write("self.cgroup", "3:memory:/docker/abc\n");
   Write("cgroup/memory/docker/abc/memory.limit_in_bytes",
         "9223372036854771712\n");
   EXPECT_EQ(Limit(), 0u);
}

TEST_F(CgroupLimitTest, PrefersTheV2LimitInAHybridHierarchy)
{
   Write("self.cgroup", "3:memory:/v1\n0::/v2\n");
   Write("cgroup/v2/memory.max", "1073741824\n");
   Write("cgroup/memory/v1/memory.limit_in_bytes", "536870912\n");
   EXPECT_EQ(Limit(), 1024 * kMiB);
}

TEST_F(CgroupLimitTest, GivesNoLimitWithoutTheCgroupFileOfTheProcess)
{
   Write("cgroup/memory.max", "1073741824\n");
   EXPECT_EQ(Limit(), 0u);
}

// Called once at start-up and again from the config path, and it must not cache
// a stale answer across them.
//
TEST(Memory, CgroupLimitIsRepeatable)
{
   EXPECT_EQ(XrdMonCgroupLimit(), XrdMonCgroupLimit());
}

// Both are called on paths that run repeatedly (the control tick) or twice
// across the two daemon modes, so neither may be one-shot or fragile.
//
TEST(Memory, ReleaseAndTuneAreRepeatable)
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
TEST(Memory, RecycleKeepsOrdinaryBodyWarm)
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
TEST(Memory, RecycleReleasesOutlierCapacity)
{
   std::string b(8 * kMiB, 'x');
   ASSERT_GT(b.capacity(), kBodyKeepBytes);

   XrdMonRecycleBody(b);

   EXPECT_TRUE(b.empty());
   EXPECT_LE(b.capacity(), kBodyKeepBytes);
}
