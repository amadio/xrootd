/****************************************************************
 *
 * Copyright (C) 2025, Pelican Project, Morgridge Institute for Research
 *
 ***************************************************************/

#include "../XrdClCurlCommon/TransferTest.hh"

#include <XrdCl/XrdClFileSystem.hh>

class CurlDeleteFixture : public TransferFixture {};

TEST_F(CurlDeleteFixture, Test)
{
    std::string fname = "/test/delete_file";
    auto url = GetOriginURL() + fname;
    WritePattern(url, 8, 'a', 2);
    XrdCl::FileSystem fs(GetOriginURL());

    XrdCl::StatInfo *response{nullptr};
    auto st = fs.Stat(fname + "?authz=" + GetReadToken(), response, 10);
    ASSERT_TRUE(st.IsOK()) << "Failed to stat new file: " << st.ToString();
    delete response;

    st = fs.Rm(fname + "?authz=" + GetWriteToken(), 10);
    ASSERT_TRUE(st.IsOK()) << "Failed to remove file: " << st.ToString();

    response = nullptr;
    st = fs.Stat(fname + "?authz=" + GetReadToken(), response, 10);
    ASSERT_FALSE(st.IsOK()) << "Stat of removed file should have failed";
    ASSERT_EQ(st.errNo, kXR_NotFound);
}