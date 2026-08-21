//------------------------------------------------------------------------------
// Unit tests for XrdMonDiskCache: the on-failure disk cache that stores failed
// OpenSearch _bulk bodies and replays them oldest-first, surviving restarts.
//------------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "XrdApps/XrdMonCollect/XrdMonDiskCache.hh"

#include <gtest/gtest.h>

namespace
{
// ::testing::TempDir() is not public in the GoogleTest shipped with EL8.
std::string tempDir()
{
   const char* dir = std::getenv("TMPDIR");
   std::string path = (dir && *dir) ? dir : "/tmp";
   if (path.back() != '/') path += '/';
   return path;
}

// A fresh, empty cache directory under the test's temp dir.
std::string freshDir(const std::string& name)
{
   std::string d = tempDir() + name;
   // Best-effort clean: remove known files from a prior run.
   std::string cmd = "rm -rf '" + d + "'";
   if (system(cmd.c_str()) != 0) { /* ignore */ }
   return d;
}

// A replay callback that records the bodies it received and returns a fixed
// result (true = POST succeeded, false = sink still down).
struct Recorder
{
   std::vector<std::string> seen;
   bool result = true;
   bool operator()(const std::string& b) { seen.push_back(b); return result; }
};

// Names of the files in `dir` ending in `suffix` (sorted for stable asserts).
std::vector<std::string> filesWithSuffix(const std::string& dir,
                                         const std::string& suffix)
{
   std::vector<std::string> out;
   std::string cmd = "ls -1 '" + dir + "' 2>/dev/null";
   FILE* p = popen(cmd.c_str(), "r");
   if (!p) return out;
   char line[512];
   while (fgets(line, sizeof(line), p))
      {std::string n = line;
       while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
       if (n.size() >= suffix.size() &&
           n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
          out.push_back(n);
      }
   pclose(p);
   return out;
}
}

TEST(XrdMonDiskCache, StoreThenReplayInFifoOrder)
{
   XrdMonDiskCache c(freshDir("dc-fifo"));
   std::string e;
   ASSERT_TRUE(c.Init(e)) << e;

   ASSERT_TRUE(c.Store("body-1", e)) << e;
   ASSERT_TRUE(c.Store("body-2", e)) << e;
   ASSERT_TRUE(c.Store("body-3", e)) << e;
   EXPECT_EQ(c.Files(), 3u);
   EXPECT_FALSE(c.Empty());
   EXPECT_EQ(c.Stored(), 3u);
   EXPECT_GT(c.Bytes(), 0u);

   Recorder rec;  // succeeds
   auto cb = [&](const std::string& b){ return rec(b); };
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c.ReplayOldest(cb, e), 0);   // nothing left

   ASSERT_EQ(rec.seen.size(), 3u);
   EXPECT_EQ(rec.seen[0], "body-1");      // oldest first
   EXPECT_EQ(rec.seen[1], "body-2");
   EXPECT_EQ(rec.seen[2], "body-3");
   EXPECT_TRUE(c.Empty());
   EXPECT_EQ(c.Files(), 0u);
   EXPECT_EQ(c.Bytes(), 0u);
   EXPECT_EQ(c.Replayed(), 3u);
}

TEST(XrdMonDiskCache, FailedReplayKeepsFile)
{
   XrdMonDiskCache c(freshDir("dc-keep"));
   std::string e;
   ASSERT_TRUE(c.Init(e)) << e;
   ASSERT_TRUE(c.Store("payload", e)) << e;

   Recorder down; down.result = false;    // sink still unavailable
   auto cbDown = [&](const std::string& b){ return down(b); };
   EXPECT_EQ(c.ReplayOldest(cbDown, e), -1);
   EXPECT_EQ(c.Files(), 1u);              // file is kept for a later attempt
   EXPECT_FALSE(c.Empty());
   EXPECT_EQ(c.Replayed(), 0u);

   Recorder up;                           // sink recovered
   auto cbUp = [&](const std::string& b){ return up(b); };
   EXPECT_EQ(c.ReplayOldest(cbUp, e), 1);
   EXPECT_EQ(up.seen.size(), 1u);
   EXPECT_EQ(up.seen[0], "payload");
   EXPECT_TRUE(c.Empty());
   EXPECT_EQ(c.Replayed(), 1u);
}

TEST(XrdMonDiskCache, SurvivesRestart)
{
   std::string dir = freshDir("dc-restart");
   std::string e;
   {  XrdMonDiskCache c(dir);
      ASSERT_TRUE(c.Init(e)) << e;
      ASSERT_TRUE(c.Store("a", e)) << e;
      ASSERT_TRUE(c.Store("b", e)) << e;
   }
   // A new instance on the same directory recovers the pending backlog.
   XrdMonDiskCache c2(dir);
   ASSERT_TRUE(c2.Init(e)) << e;
   EXPECT_EQ(c2.Files(), 2u);
   EXPECT_GT(c2.Bytes(), 0u);

   Recorder rec;
   auto cb = [&](const std::string& b){ return rec(b); };
   EXPECT_EQ(c2.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c2.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c2.ReplayOldest(cb, e), 0);
   ASSERT_EQ(rec.seen.size(), 2u);
   EXPECT_EQ(rec.seen[0], "a");           // order preserved across restart
   EXPECT_EQ(rec.seen[1], "b");
   EXPECT_TRUE(c2.Empty());
}

TEST(XrdMonDiskCache, InitRemovesStalePartials)
{
   std::string dir = freshDir("dc-tmp");
   std::string e;
   {  XrdMonDiskCache c(dir);
      ASSERT_TRUE(c.Init(e)) << e;        // creates the directory
   }
   // Leave a stale .tmp partial (as a crash mid-write would) and a good file.
   std::ofstream(dir + "/0000000000001-000000.ndjson.tmp") << "partial";
   {  XrdMonDiskCache c(dir);
      ASSERT_TRUE(c.Init(e)) << e;
      ASSERT_TRUE(c.Store("good", e)) << e;
   }
   // Re-open: the .tmp must have been removed; only the good body remains.
   XrdMonDiskCache c2(dir);
   ASSERT_TRUE(c2.Init(e)) << e;
   EXPECT_EQ(c2.Files(), 1u);
   Recorder rec;
   auto cb = [&](const std::string& b){ return rec(b); };
   EXPECT_EQ(c2.ReplayOldest(cb, e), 1);
   ASSERT_EQ(rec.seen.size(), 1u);
   EXPECT_EQ(rec.seen[0], "good");
}

TEST(XrdMonDiskCache, CustomSuffixStoredAndRescanned)
{
   std::string dir = freshDir("dc-suffix");
   std::string e;
   {  XrdMonDiskCache c(dir, ".frames");
      ASSERT_TRUE(c.Init(e)) << e;
      ASSERT_TRUE(c.Store("framed-bytes", e)) << e;
   }
   // A rescan with the same suffix recovers the body; the default-suffix
   // cache sharing the directory does not see it.
   XrdMonDiskCache same(dir, ".frames");
   ASSERT_TRUE(same.Init(e)) << e;
   EXPECT_EQ(same.Files(), 1u);

   XrdMonDiskCache other(dir);
   ASSERT_TRUE(other.Init(e)) << e;
   EXPECT_EQ(other.Files(), 0u);

   Recorder rec;
   auto cb = [&](const std::string& b){ return rec(b); };
   EXPECT_EQ(same.ReplayOldest(cb, e), 1);
   ASSERT_EQ(rec.seen.size(), 1u);
   EXPECT_EQ(rec.seen[0], "framed-bytes");
}

// A permanently refused body must not pin the backlog: it is moved aside as a
// .rejected file (kept for post-mortem) and the bodies behind it drain. This is
// the regression guard for the OTLP export wedging on a single body an OTLP
// receiver rejects with a validation error (e.g. Loki answering 400).
TEST(XrdMonDiskCache, RejectedReplayQuarantinesAndDrains)
{
   std::string dir = freshDir("dc-reject");
   XrdMonDiskCache c(dir);
   std::string e;
   ASSERT_TRUE(c.Init(e)) << e;
   ASSERT_TRUE(c.Store("poison", e)) << e;
   ASSERT_TRUE(c.Store("good", e)) << e;

   std::vector<std::string> seen;
   auto cb = [&](const std::string& b)
      {seen.push_back(b);
       return b == "poison" ? XrdMonDiskCache::Verdict::Rejected
                            : XrdMonDiskCache::Verdict::Delivered;
      };

   // The rejection is progress (1, not -1): the head leaves the backlog.
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   EXPECT_NE(e.find("quarantined"), std::string::npos) << e;
   e.clear();
   EXPECT_EQ(c.Files(), 1u);
   EXPECT_EQ(c.Rejected(), 1u);
   EXPECT_EQ(c.Replayed(), 0u);

   // The body behind the poison one is now deliverable.
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c.ReplayOldest(cb, e), 0);
   ASSERT_EQ(seen.size(), 2u);
   EXPECT_EQ(seen[0], "poison");
   EXPECT_EQ(seen[1], "good");
   EXPECT_TRUE(c.Empty());
   EXPECT_EQ(c.Replayed(), 1u);

   // The evidence is on disk, and a restart does not resurrect it as backlog.
   EXPECT_EQ(filesWithSuffix(dir, ".rejected").size(), 1u);
   XrdMonDiskCache c2(dir);
   ASSERT_TRUE(c2.Init(e)) << e;
   EXPECT_EQ(c2.Files(), 0u);
   EXPECT_EQ(filesWithSuffix(dir, ".rejected").size(), 1u);
}

// The live-path form: a body the sink has already refused goes straight to
// quarantine without ever entering the pending backlog.
TEST(XrdMonDiskCache, QuarantineBypassesBacklog)
{
   std::string dir = freshDir("dc-quarantine");
   XrdMonDiskCache c(dir);
   std::string e;
   ASSERT_TRUE(c.Init(e)) << e;

   ASSERT_TRUE(c.Quarantine("refused-body", e));
   EXPECT_NE(e.find("quarantined"), std::string::npos) << e;
   EXPECT_TRUE(c.Empty());
   EXPECT_EQ(c.Files(), 0u);
   EXPECT_EQ(c.Bytes(), 0u);
   EXPECT_EQ(c.Rejected(), 1u);

   auto names = filesWithSuffix(dir, ".rejected");
   ASSERT_EQ(names.size(), 1u);
   std::ifstream f(dir + "/" + names[0]);
   std::string content((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
   EXPECT_EQ(content, "refused-body");
}

TEST(XrdMonDiskCache, MaxBytesDropsOldest)
{
   XrdMonDiskCache c(freshDir("dc-cap"));
   c.SetMaxBytes(10);
   std::string e;
   ASSERT_TRUE(c.Init(e)) << e;

   ASSERT_TRUE(c.Store("aaaa", e)) << e;    // 4 bytes
   ASSERT_TRUE(c.Store("bbbb", e)) << e;    // 8 bytes total
   EXPECT_EQ(c.Dropped(), 0u);
   ASSERT_TRUE(c.Store("cccc", e)) << e;    // would be 12: drops "aaaa"
   EXPECT_EQ(c.Dropped(), 1u);
   EXPECT_EQ(c.Files(), 2u);
   EXPECT_LE(c.Bytes(), 10u);

   // A body larger than what remains evicts everything older, then stores.
   ASSERT_TRUE(c.Store("ddddddddd", e)) << e;   // 9 bytes: drops both
   EXPECT_EQ(c.Dropped(), 3u);
   EXPECT_EQ(c.Files(), 1u);

   Recorder rec;
   auto cb = [&](const std::string& b){ return rec(b); };
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   EXPECT_EQ(c.ReplayOldest(cb, e), 0);
   ASSERT_EQ(rec.seen.size(), 1u);
   EXPECT_EQ(rec.seen[0], "ddddddddd");     // only the newest survived
}

// Several destinations nest their caches under one --cache-dir, so a sibling
// cache's directory can sit inside this one's scan. Adopting it as a body
// would have the replay read a directory, fail, and unlink it -- taking the
// other destination's whole backlog with it.
TEST(XrdMonDiskCache, IgnoresSubdirectoriesInTheScan)
{
   const std::string d = freshDir("dc-nested");
   {XrdMonDiskCache seed(d);
    std::string e;
    ASSERT_TRUE(seed.Init(e)) << e;
    ASSERT_TRUE(seed.Store("real-body", e)) << e;
   }
   // A sibling destination's cache, named so it matches the default suffix.
   ASSERT_EQ(system(("mkdir -p '" + d + "/os-central.ndjson'").c_str()), 0);

   XrdMonDiskCache c(d);
   std::string e;
   ASSERT_TRUE(c.Init(e)) << e;
   EXPECT_EQ(c.Files(), 1u);

   Recorder rec;
   auto cb = [&](const std::string& b){ return rec(b); };
   EXPECT_EQ(c.ReplayOldest(cb, e), 1);
   ASSERT_EQ(rec.seen.size(), 1u);
   EXPECT_EQ(rec.seen[0], "real-body");
   EXPECT_EQ(c.ReplayOldest(cb, e), 0);         // nothing else was adopted

   // The sibling's directory is still there.
   EXPECT_EQ(system(("test -d '" + d + "/os-central.ndjson'").c_str()), 0);
}
