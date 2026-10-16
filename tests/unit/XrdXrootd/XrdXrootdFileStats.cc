//------------------------------------------------------------------------------
// Unit tests for XrdXrootdFileStats: the first error of a file, which the
// f-stream close record reports. These tests do not start the monitor.
//------------------------------------------------------------------------------

#include "XProtocol/XProtocol.hh"
#include "XrdXrootd/XrdXrootdFileStats.hh"

#include <gtest/gtest.h>

// A file without an f-stream open record gets no close record, so its errors
// are not kept
TEST(XrdXrootdFileStats, SetErrIgnoresAFileWithoutAMonitorEntry)
{
  XrdXrootdFileStats stats;
  stats.Init();
  stats.setErr(kXR_IOError, monErrRead);
  EXPECT_EQ(stats.errCat, 0);
}

// The error of a file with a monitor entry is kept
TEST(XrdXrootdFileStats, SetErrKeepsTheCodeAndTheCategory)
{
  XrdXrootdFileStats stats;
  stats.Init();
  stats.MonEnt = 0;
  stats.setErr(kXR_IOError, monErrRead);
  EXPECT_EQ(stats.errCode, kXR_IOError);
  EXPECT_EQ(stats.errCat, monErrRead);
}

// A later error does not replace the first error
TEST(XrdXrootdFileStats, SetErrKeepsOnlyTheFirstError)
{
  XrdXrootdFileStats stats;
  stats.Init();
  stats.MonEnt = 0;
  stats.setErr(kXR_IOError, monErrRead);
  stats.setErr(kXR_FSError, monErrClose);
  EXPECT_EQ(stats.errCode, kXR_IOError);
  EXPECT_EQ(stats.errCat, monErrRead);
}
