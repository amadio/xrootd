//------------------------------------------------------------------------------
// Unit tests for XrdMonBulkAdd: the OpenSearch _bulk action framing. It is a
// free function precisely so the --bulk file sink and the network sink cannot
// drift apart, and so a build without libcurl still frames files correctly --
// these tests need neither a cluster nor libcurl.
//------------------------------------------------------------------------------

#include "XrdApps/XrdMonCollect/XrdMonOpenSearch.hh"

#include <algorithm>

#include <gtest/gtest.h>

TEST(XrdMonBulk, IndexActionForAPlainIndex)
{
   std::string b;
   XrdMonBulkAdd(b, "xrootd-file-ops", false, "{\"a\":1}");
   EXPECT_EQ(b, "{\"index\":{\"_index\":\"xrootd-file-ops\"}}\n{\"a\":1}\n");
}

// Data streams reject the "index" action outright, so --os-datastream has to
// reach the framing, not just the connection.
TEST(XrdMonBulk, CreateActionForADataStream)
{
   std::string b;
   XrdMonBulkAdd(b, "xrootd-file-ops", true, "{\"a\":1}");
   EXPECT_EQ(b, "{\"create\":{\"_index\":\"xrootd-file-ops\"}}\n{\"a\":1}\n");
}

// A body is accumulated across calls, and every record is newline-terminated:
// _bulk is NDJSON, and a missing final newline is an error, not a nicety.
TEST(XrdMonBulk, AppendsAndTerminatesEveryRecord)
{
   std::string b;
   XrdMonBulkAdd(b, "idx", false, "{\"a\":1}");
   XrdMonBulkAdd(b, "idx", false, "{\"b\":2}");

   EXPECT_EQ(std::count(b.begin(), b.end(), '\n'), 4);
   EXPECT_EQ(b.back(), '\n');
   EXPECT_NE(b.find("{\"b\":2}"), std::string::npos);
}
