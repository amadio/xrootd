#include "XrdHttpS3List.hh"

#include <gtest/gtest.h>

using namespace XrdHttpS3;

namespace
{

ListRequest Request(const std::string &prefix, int maxKeys = MaxKeys)
{
  ListRequest req;
  req.bucket = "bucket";
  req.prefix = prefix;
  req.delimiter = "/";
  req.maxKeys = maxKeys;
  return req;
}

// The entries a.txt (3 bytes), b/ and c.txt (5 bytes), not in order
std::vector<Entry> Entries()
{
  return {{"c.txt", false, 5, 0}, {"b", true, 0, 0}, {"a.txt", false, 3, 0}};
}

bool Has(const std::string &xml, const std::string &text)
{
  return xml.find(text) != std::string::npos;
}

} // namespace

TEST(ListQueryTest, ParsesTheParametersOfAListing)
{
  ListRequest req;
  std::string err;
  ASSERT_TRUE(ParseListQuery("list-type=2&delimiter=%2F&encoding-type=url"
                             "&prefix=a%20b%26c%2F&max-keys=10"
                             "&continuation-token=x%2By&start-after=s",
                             req, err)) << err;
  EXPECT_EQ(req.delimiter, "/");
  EXPECT_TRUE(req.urlEncoding);
  EXPECT_EQ(req.prefix, "a b&c/");
  EXPECT_EQ(req.maxKeys, 10);
  EXPECT_EQ(req.continuationToken, "x+y");
  EXPECT_EQ(req.startAfter, "s");
}

TEST(ListQueryTest, IgnoresAParameterWhichItDoesNotKnow)
{
  ListRequest req;
  std::string err;
  EXPECT_TRUE(ParseListQuery("fetch-owner=true&&list-type=2&x", req, err));
}

TEST(ListQueryTest, LimitsTheNumberOfKeysTo1000)
{
  ListRequest req;
  std::string err;
  ASSERT_TRUE(ParseListQuery("list-type=2&max-keys=5000", req, err));
  EXPECT_EQ(req.maxKeys, 1000);
}

TEST(ListQueryTest, RejectsAnInvalidQuery)
{
  for (const char *query : {"", "prefix=a", "list-type=1", "list-type=2&max-keys=",
                            "list-type=2&max-keys=-1", "list-type=2&max-keys=1x",
                            "list-type=2&encoding-type=base64"}) {
    ListRequest req;
    std::string err;
    EXPECT_FALSE(ParseListQuery(query, req, err)) << query;
    EXPECT_FALSE(err.empty()) << query;
  }
}

TEST(SplitPrefixTest, SplitsAtTheLastSlash)
{
  std::string dir, start;
  ASSERT_TRUE(SplitPrefix("a/b/c", dir, start));
  EXPECT_EQ(dir, "a/b/");
  EXPECT_EQ(start, "c");
  ASSERT_TRUE(SplitPrefix("a/", dir, start));
  EXPECT_EQ(dir, "a/");
  EXPECT_EQ(start, "");
  ASSERT_TRUE(SplitPrefix("abc", dir, start));
  EXPECT_EQ(dir, "");
  EXPECT_EQ(start, "abc");
  ASSERT_TRUE(SplitPrefix("", dir, start));
  EXPECT_EQ(dir, "");
}

TEST(SplitPrefixTest, RejectsADirectoryWhichCannotExist)
{
  std::string dir, start;
  for (const char *prefix : {"/a", "a//b", "../a", "a/./b", "a/../b/"})
    EXPECT_FALSE(SplitPrefix(prefix, dir, start)) << prefix;
  // Only the directory is a name of the file system
  EXPECT_TRUE(SplitPrefix("a/..", dir, start));
}

TEST(ListObjectsV2Test, ListsTheObjectsAndTheDirectoriesInOrder)
{
  auto xml = ListObjectsV2(Request(""), "", Entries(), true, 0);
  EXPECT_TRUE(Has(xml, "<ListBucketResult xmlns="));
  EXPECT_TRUE(Has(xml, "<Name>bucket</Name>"));
  EXPECT_TRUE(Has(xml, "<KeyCount>3</KeyCount>"));
  EXPECT_TRUE(Has(xml, "<IsTruncated>false</IsTruncated>"));
  EXPECT_FALSE(Has(xml, "NextContinuationToken"));
  EXPECT_TRUE(Has(xml, "<Contents><Key>a.txt</Key><LastModified>"
                       "1970-01-01T00:00:00.000Z</LastModified><Size>3</Size>"));
  EXPECT_LT(xml.find("<Key>a.txt"), xml.find("<Key>c.txt"));
  EXPECT_TRUE(Has(xml, "<CommonPrefixes><Prefix>b/</Prefix></CommonPrefixes>"));
  // The common prefixes come after the objects
  EXPECT_LT(xml.find("<Key>c.txt"), xml.find("<CommonPrefixes>"));
}

TEST(ListObjectsV2Test, ListsOnlyTheEntriesWhichStartWithThePrefix)
{
  auto xml = ListObjectsV2(Request("d/a"), "d/", Entries(), true, 0);
  EXPECT_TRUE(Has(xml, "<KeyCount>1</KeyCount>"));
  EXPECT_TRUE(Has(xml, "<Key>d/a.txt</Key>"));
  EXPECT_FALSE(Has(xml, "c.txt"));
}

TEST(ListObjectsV2Test, HasTheKeyOfTheDirectoryOfThePrefix)
{
  auto xml = ListObjectsV2(Request("d/"), "d/", {}, true, 0);
  EXPECT_TRUE(Has(xml, "<KeyCount>1</KeyCount>"));
  EXPECT_TRUE(Has(xml, "<Key>d/</Key>"));
}

TEST(ListObjectsV2Test, IsEmptyForADirectoryWhichDoesNotExist)
{
  auto xml = ListObjectsV2(Request("d/"), "d/", {}, false, 0);
  EXPECT_TRUE(Has(xml, "<KeyCount>0</KeyCount>"));
  EXPECT_FALSE(Has(xml, "<Contents>"));
}

TEST(ListObjectsV2Test, PagesFollowTheToken)
{
  auto req = Request("", 2);
  auto xml = ListObjectsV2(req, "", Entries(), true, 0);
  EXPECT_TRUE(Has(xml, "<IsTruncated>true</IsTruncated>"));
  EXPECT_TRUE(Has(xml, "<NextContinuationToken>b/</NextContinuationToken>"));
  EXPECT_TRUE(Has(xml, "<Key>a.txt</Key>"));
  EXPECT_FALSE(Has(xml, "c.txt"));

  req.continuationToken = "b/";
  xml = ListObjectsV2(req, "", Entries(), true, 0);
  EXPECT_TRUE(Has(xml, "<IsTruncated>false</IsTruncated>"));
  EXPECT_TRUE(Has(xml, "<ContinuationToken>b/</ContinuationToken>"));
  EXPECT_TRUE(Has(xml, "<KeyCount>1</KeyCount>"));
  EXPECT_TRUE(Has(xml, "<Key>c.txt</Key>"));
}

TEST(ListObjectsV2Test, StartsAfterTheKeyOfStartAfter)
{
  auto req = Request("");
  req.startAfter = "a.txt";
  auto xml = ListObjectsV2(req, "", Entries(), true, 0);
  EXPECT_TRUE(Has(xml, "<KeyCount>2</KeyCount>"));
  EXPECT_FALSE(Has(xml, "<Key>a.txt</Key>"));
}

TEST(ListObjectsV2Test, NoKeysIsNotATruncatedListing)
{
  auto xml = ListObjectsV2(Request("", 0), "", Entries(), true, 0);
  EXPECT_TRUE(Has(xml, "<KeyCount>0</KeyCount>"));
  EXPECT_TRUE(Has(xml, "<IsTruncated>false</IsTruncated>"));
}

TEST(ListObjectsV2Test, EncodesTheKeysForTheUrlEncoding)
{
  auto req = Request("d d/");
  req.urlEncoding = true;
  auto xml = ListObjectsV2(req, "d d/", {{"a&b", false, 1, 0}}, true, 0);
  EXPECT_TRUE(Has(xml, "<EncodingType>url</EncodingType>"));
  EXPECT_TRUE(Has(xml, "<Prefix>d%20d%2F</Prefix>"));
  EXPECT_TRUE(Has(xml, "<Key>d%20d%2Fa%26b</Key>"));
}

TEST(ListObjectsV2Test, EscapesTheKeysForXml)
{
  auto xml = ListObjectsV2(Request(""), "", {{"a&<b>", false, 1, 0}}, true, 0);
  EXPECT_TRUE(Has(xml, "<Key>a&amp;&lt;b&gt;</Key>"));
}

TEST(ErrorXmlTest, HasTheCodeTheMessageAndTheResource)
{
  auto xml = ErrorXml("NoSuchBucket", "No \"bucket\"", "/b");
  EXPECT_TRUE(Has(xml, "<Error><Code>NoSuchBucket</Code>"
                       "<Message>No &quot;bucket&quot;</Message>"
                       "<Resource>/b</Resource></Error>"));
}

TEST(IsoTimeTest, FormatsTheTimeInUtc)
{
  EXPECT_EQ(IsoTime(1692615773), "2023-08-21T11:02:53.000Z");
}
