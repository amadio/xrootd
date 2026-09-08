#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern unsigned long XrdOucHashVal(const char *KeyVal);
extern unsigned long XrdOucHashVal2(const char *KeyVal, int KeyLen);

namespace {

// Reference: the key length, XOR'ed with the first word when the length is
// not a multiple of the word size, XOR'ed with every aligned word after the
// remainder. This is the documented algorithm of XrdOucHashVal2().
unsigned long ReferenceHash(const std::string &key)
{
  const int hl = sizeof(unsigned long);
  int len = key.size();
  unsigned long hval = len, word;
  int j = len % hl;
  if (j) {
    std::memcpy(&word, key.data(), hl);
    hval ^= word;
  }
  for (const char *cp = key.data() + j; cp < key.data() + len; cp += hl) {
    std::memcpy(&word, cp, hl);
    hval ^= word;
  }
  return hval ? hval : 1;
}

} // namespace

TEST(XrdOucHashVal, MatchesReferenceAlgorithm)
{
  const std::string key = "MaxBuffSize/with/a/longer/key";
  EXPECT_EQ(XrdOucHashVal(key.c_str()), ReferenceHash(key));
}

TEST(XrdOucHashVal, MisalignedKeyHashesLikeAlignedKey)
{
  // XrdOucEnv hashes names that sit at arbitrary offsets inside a larger
  // buffer, which UBSan flagged as a misaligned unsigned long load.
  const std::string key = "MaxBuffSize/with/a/longer/key";
  alignas(unsigned long) char buffer[64] = {};
  std::memcpy(buffer + 3, key.data(), key.size());
  EXPECT_EQ(XrdOucHashVal2(buffer + 3, key.size()), XrdOucHashVal(key.c_str()));
}

TEST(XrdOucHashVal, ShortKeyIsItsOwnBytes)
{
  unsigned long expected = 0;
  std::memcpy(&expected, "abc", 3);
  EXPECT_EQ(XrdOucHashVal("abc"), expected);
}
