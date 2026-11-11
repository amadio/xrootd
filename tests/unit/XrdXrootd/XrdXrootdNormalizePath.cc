//------------------------------------------------------------------------------
// Tests of XrdXrootd::NormalizePath, which applies the path checks of a
// native request to the path of another front end. The HTTP third-party-copy
// handler opens local files directly, so it must refuse a path with a ".."
// component and a path outside the exported paths, and it must open the
// normalized path.
//------------------------------------------------------------------------------

#include <cstring>
#include <string>

#include <gtest/gtest.h>

// The headers below are not self-contained and need the headers above
#include "XrdXrootd/XrdXrootdXPath.hh"
#include "XrdXrootd/XrdXrootdProtocol.hh"

namespace {

// The list of exported paths is a protected static member of the protocol.
// The tests never make an object of this class.
struct Exports : XrdXrootdProtocol {
   static void Add(const char *path) { XPList.Insert(path); }
};

class NormalizePathTest : public ::testing::Test {
protected:
   static void SetUpTestSuite()
   {
      Exports::Add("/data");
      Exports::Add("/store/user");
   }

   // The normalized path, or "refused"
   static std::string Normalize(std::string path)
   {
      return XrdXrootd::NormalizePath(path) ? path : "refused";
   }
};

TEST_F(NormalizePathTest, KeepsAPathInsideAnExport)
{
   EXPECT_EQ(Normalize("/store/user/file"), "/store/user/file");
}

TEST_F(NormalizePathTest, KeepsTheSlashAtTheEnd)
{
   EXPECT_EQ(Normalize("/data/dir/"), "/data/dir/");
}

TEST_F(NormalizePathTest, RemovesDuplicateSlashesAndDotComponents)
{
   EXPECT_EQ(Normalize("/./data//./private/file"), "/data/private/file");
}

TEST_F(NormalizePathTest, RemovesTheOpaquePart)
{
   EXPECT_EQ(Normalize("/data/file?a=1&b=2"), "/data/file");
}

TEST_F(NormalizePathTest, IgnoresDotDotInTheOpaquePart)
{
   EXPECT_EQ(Normalize("/data?/../etc"), "/data");
}

TEST_F(NormalizePathTest, RefusesAPathOutsideTheExports)
{
   EXPECT_EQ(Normalize("/etc/passwd"), "refused");
}

TEST_F(NormalizePathTest, RefusesADotDotComponentThatLeavesTheExport)
{
   EXPECT_EQ(Normalize("/data/../etc/passwd"), "refused");
}

TEST_F(NormalizePathTest, RefusesADotDotComponentThatStaysInTheExport)
{
   EXPECT_EQ(Normalize("/data/dir/../file"), "refused");
}

// A decoded "%3F" in an HTTP request path is a '?'; it must not hide a ".."
TEST_F(NormalizePathTest, RefusesADotDotComponentBeforeTheOpaquePart)
{
   EXPECT_EQ(Normalize("/data/..?"), "refused");
}

TEST_F(NormalizePathTest, RefusesAPathThatIsNotAbsolute)
{
   EXPECT_EQ(Normalize("data/file"), "refused");
}

TEST_F(NormalizePathTest, RefusesAnEmptyPath)
{
   EXPECT_EQ(Normalize(""), "refused");
}

}
