//------------------------------------------------------------------------------
// Unit tests for the collector's shared helpers: the UTF-8 repair applied to
// every string arriving off the monitoring wire, and the serializer that must
// never throw. XRootD carries path and identity bytes verbatim, so both of
// these are load-bearing -- a byte sequence JSON cannot represent used to take
// the whole process down.
//------------------------------------------------------------------------------

#include <string>

#include "XrdApps/XrdMonCollect/XrdMonUtil.hh"
#include "XrdOuc/XrdOucJson.hh"

#include <gtest/gtest.h>

using json = nlohmann::json;

namespace
{
const char* const kFFFD = "\xef\xbf\xbd";     // U+FFFD REPLACEMENT CHARACTER

// Repair a copy and report both what came out and whether anything was done.
std::string cleaned(const std::string& in, bool* changed = nullptr)
{
   std::string s = in;
   bool did = XrdMonUtf8Clean(s);
   if (changed) *changed = did;
   return s;
}

// Whatever comes out must itself be valid, i.e. a second pass is a no-op.
::testing::AssertionResult isClean(const std::string& s)
{
   std::string copy = s;
   if (!XrdMonUtf8Clean(copy)) return ::testing::AssertionSuccess();
   return ::testing::AssertionFailure() << "output still needs repair";
}
}

TEST(XrdMonUtf8, LeavesAsciiAlone)
{
  bool changed = true;
  const std::string lfn = "/store/data/2024/AOD-00123_step2.root";
  EXPECT_EQ(cleaned(lfn, &changed), lfn);
  EXPECT_FALSE(changed);

  changed = true;
  EXPECT_EQ(cleaned("", &changed), "");
  EXPECT_FALSE(changed);
}

TEST(XrdMonUtf8, LeavesValidMultiByteAlone)
{
  bool changed = true;
  // Two, three and four byte sequences: é, €, and U+1D11E (musical G clef).
  const std::string s = "/store/caf\xc3\xa9/\xe2\x82\xac/\xf0\x9d\x84\x9e.root";
  EXPECT_EQ(cleaned(s, &changed), s);
  EXPECT_FALSE(changed);
}

TEST(XrdMonUtf8, ReplacesEachStrayByte)
{
  bool changed = false;
  // 0xff can never appear in UTF-8 at all.
  EXPECT_EQ(cleaned("a\xff" "b", &changed), std::string("a") + kFFFD + "b");
  EXPECT_TRUE(changed);

  // One replacement per offending byte, so a run stays visible as a run.
  EXPECT_EQ(cleaned("\xff\xfe"), std::string(kFFFD) + kFFFD);
}

TEST(XrdMonUtf8, ReplacesTruncatedSequences)
{
  // A three-byte lead with only one continuation, at the end of the string:
  // the LFN was cut by strnlen at the record boundary.
  EXPECT_EQ(cleaned("ok/\xe2\x82"), std::string("ok/") + kFFFD + kFFFD);

  // A lead byte followed by something that is not a continuation.
  EXPECT_EQ(cleaned("\xc3/"), std::string(kFFFD) + "/");
}

TEST(XrdMonUtf8, ReplacesBareContinuationBytes)
{
  EXPECT_EQ(cleaned("\x80"), kFFFD);
  EXPECT_EQ(cleaned("x\xbf" "y"), std::string("x") + kFFFD + "y");
}

// The three families nlohmann rejects that a naive "lead plus continuations"
// check would wave through, and that therefore have to be caught here too.
TEST(XrdMonUtf8, ReplacesOverlongSurrogateAndOutOfRange)
{
  EXPECT_EQ(cleaned("\xc0\x80"), std::string(kFFFD) + kFFFD);       // overlong NUL
  EXPECT_EQ(cleaned("\xc1\xbf"), std::string(kFFFD) + kFFFD);       // overlong
  EXPECT_EQ(cleaned("\xe0\x80\xaf"),
            std::string(kFFFD) + kFFFD + kFFFD);                    // overlong
  EXPECT_EQ(cleaned("\xed\xa0\x80"),
            std::string(kFFFD) + kFFFD + kFFFD);                    // D800 surrogate
  EXPECT_EQ(cleaned("\xf0\x8f\xbf\xbf"),
            std::string(kFFFD) + kFFFD + kFFFD + kFFFD);            // overlong
  EXPECT_EQ(cleaned("\xf4\x90\x80\x80"),
            std::string(kFFFD) + kFFFD + kFFFD + kFFFD);            // past U+10FFFF
  EXPECT_EQ(cleaned("\xf5\x80\x80\x80"),
            std::string(kFFFD) + kFFFD + kFFFD + kFFFD);            // past U+10FFFF
}

// Latin-1, which is what a mis-encoded filename usually is in practice: every
// high byte is invalid on its own, and everything around it must survive.
TEST(XrdMonUtf8, KeepsTheValidPartsOfALatin1Name)
{
  bool changed = false;
  const std::string s = "/store/user/j\xe9r\xf4me/data.root";
  const std::string got = cleaned(s, &changed);

  EXPECT_TRUE(changed);
  EXPECT_EQ(got, std::string("/store/user/j") + kFFFD + "r" + kFFFD
                 + "me/data.root");
  EXPECT_TRUE(isClean(got));
}

TEST(XrdMonUtf8, OutputIsAlwaysValid)
{
  // Every lead byte, in isolation and followed by one plausible continuation.
  for (int c = 0x80; c <= 0xff; c++)
      {std::string one(1, (char)c);
       EXPECT_TRUE(isClean(cleaned(one))) << "lead " << std::hex << c;
       std::string two = one + "\x80";
       EXPECT_TRUE(isClean(cleaned(two))) << "lead " << std::hex << c << " +80";
      }
}

// The point of all of the above: nothing the decoder builds can make the
// serializer throw, because throwing would abort the process.
TEST(XrdMonUtf8, RepairedStringsSerializeCleanly)
{
  json j;
  j["attributes"]["file.path"] = cleaned("/store/user/j\xe9r\xf4me/data.root");
  std::string text;
  ASSERT_NO_THROW(text = XrdMonDump(j));
  EXPECT_NO_THROW(json::parse(text));
}

// And the net behind it: even handed bytes that were never scrubbed, the
// serializer substitutes rather than throwing.
TEST(XrdMonUtf8, DumpSubstitutesRatherThanThrowing)
{
  json j;
  j["attributes"]["file.path"] = std::string("/store/bad\xff.root");

  EXPECT_THROW((void)j.dump(), json::type_error);      // what the default does

  std::string text;
  ASSERT_NO_THROW(text = XrdMonDump(j));
  EXPECT_NE(text.find(kFFFD), std::string::npos);
  EXPECT_NO_THROW(json::parse(text));
}
