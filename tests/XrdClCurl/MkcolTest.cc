/****************************************************************
 *
 * Copyright (C) 2025, Pelican Project, Morgridge Institute for Research
 *
 ***************************************************************/

#include "../XrdClCurlCommon/TransferTest.hh"

#include <XrdCl/XrdClFileSystem.hh>

#include <memory>

class CurlMkcolFixture : public TransferFixture {};

TEST_F(CurlMkcolFixture, Test)
{
    std::string fname = "/test/mkcol_directory";
    XrdCl::FileSystem fs(GetOriginURL());

    std::unique_ptr<XrdCl::StatInfo> response_ptr;
    XrdCl::StatInfo *response{nullptr};
    auto st = fs.Stat(fname + "?authz=" + GetReadToken(), response, 10);
    response_ptr.reset(response);
    ASSERT_FALSE(st.IsOK()) << "New directory should not already exist: " << st.ToString();
    ASSERT_EQ(st.errNo, kXR_NotFound);

    st = fs.MkDir(fname + "?authz=" + GetWriteToken(), XrdCl::MkDirFlags::None, XrdCl::Access::None, 10);
    ASSERT_TRUE(st.IsOK()) << "Failed to create directory: " << st.ToString();

    response = nullptr;
    st = fs.Stat(fname + "?authz=" + GetReadToken(), response, 10);
    response_ptr.reset(response);
    ASSERT_TRUE(st.IsOK()) << "Stat of created directory failed: " << st.ToString();
}

TEST_F(CurlMkcolFixture, MkpathTest)
{
    std::string fname = "/test/mkpath_directory/subdir1/subdir2";
    XrdCl::FileSystem fs(GetOriginURL());

    std::unique_ptr<XrdCl::StatInfo> response_ptr;
    XrdCl::StatInfo *response{nullptr};
    auto st = fs.Stat(fname + "?authz=" + GetReadToken(), response, 10);
    response_ptr.reset(response);
    ASSERT_FALSE(st.IsOK()) << "New directory should not already exist: " << st.ToString();
    ASSERT_EQ(st.errNo, kXR_NotFound);

    st = fs.MkDir(fname + "?authz=" + GetWriteToken(), XrdCl::MkDirFlags::MakePath, XrdCl::Access::None, 10);
    ASSERT_TRUE(st.IsOK()) << "Failed to create directory: " << st.ToString();

    response = nullptr;
    st = fs.Stat(fname + "?authz=" + GetReadToken(), response, 10);
    response_ptr.reset(response);
    ASSERT_TRUE(st.IsOK()) << "Stat of created directory failed: " << st.ToString();
}
