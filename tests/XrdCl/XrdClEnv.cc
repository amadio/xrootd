#undef NDEBUG

#include <XrdCl/XrdClConstants.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClEnv.hh>
#include <gtest/gtest.h>

#include <cstdlib>

using XrdCl::Env;

class XrdClEnvTest : public ::testing::Test {
protected:
  void SetUp() override {
    XrdCl::DefaultEnv::SetLogLevel("Dump");

    unsetenv("XRD_TEST_STRING");
    unsetenv("XRD_TEST_INT");
    unsetenv("XRD_TEST_INVALID");

    setenv("XRD_TEST_INT_17", "17", 1);
    setenv("XRD_TEST_STR_OK", "OK", 1);

    e.ImportInt("test_int", "XRD_TEST_INT");
    e.ImportInt("test_int_17", "XRD_TEST_INT_17");

    e.ImportString("test_string", "XRD_TEST_STRING");
    e.ImportString("test_str_ok", "XRD_TEST_STR_OK");
  }

  Env e;
};

TEST_F(XrdClEnvTest, DefaultEnv_GetLog) {
    EXPECT_TRUE(XrdCl::DefaultEnv::GetLog());
}

TEST_F(XrdClEnvTest, GetInt_Success) {
  EXPECT_TRUE(e.PutInt("XRD_TEST_INT", 42));
  int val = 0;
  EXPECT_TRUE(e.GetInt("XRD_TEST_INT", val));
  EXPECT_EQ(val, 42);

  // imported value
  EXPECT_TRUE(e.GetInt("XRD_TEST_INT_17", val));
  EXPECT_EQ(val, 17);

  // get by normalized name
  EXPECT_TRUE(e.GetInt("test_int", val));
  EXPECT_EQ(val, 42);

  // get by default value
  EXPECT_TRUE(e.GetDefaultIntValue("ParallelEvtLoop", val));
  EXPECT_EQ(val, XrdCl::DefaultParallelEvtLoop);
}

TEST_F(XrdClEnvTest, GetPutInt_Failure) {
  int val = 100;
  EXPECT_FALSE(e.GetInt("XRD_TEST_INVALID", val));
  EXPECT_EQ(val, 100); // unchanged

  // not imported, not problem to set
  EXPECT_TRUE(e.PutInt("XRD_TEST_INT", 99));

  // imported, should not override
  EXPECT_FALSE(e.PutInt("XRD_TEST_INT_17", 99));
  EXPECT_TRUE(e.GetInt("XRD_TEST_INT_17", val));
  EXPECT_EQ(val, 17);
}

TEST_F(XrdClEnvTest, GetPutString_Success) {
  std::string val;
  EXPECT_TRUE(e.PutString("XRD_TEST_STRING", "hello_env"));
  EXPECT_TRUE(e.GetString("XRD_TEST_STRING", val));
  EXPECT_EQ(val, "hello_env");

  // imported value
  EXPECT_TRUE(e.GetString("XRD_TEST_STR_OK", val));
  EXPECT_EQ(val, "OK");

  // get by normalized name
  EXPECT_TRUE(e.GetString("test_string", val));
  EXPECT_EQ(val, "hello_env");

  // get by default value
  EXPECT_TRUE(e.GetDefaultStringValue("XRD_CPRETRYPOLICY", val));
  EXPECT_EQ(XrdCl::DefaultCpRetryPolicy, val);
}

TEST_F(XrdClEnvTest, GetPutString_Failure) {
  std::string val = "initial";
  EXPECT_FALSE(e.GetString("XRD_TEST_STRING", val));
  EXPECT_EQ(val, "initial"); // unchanged

  // not imported, not problem to set
  EXPECT_TRUE(e.PutString("XRD_TEST_STRING", "OVERRIDE"));

  // imported, should not override
  EXPECT_FALSE(e.PutString("XRD_TEST_STR_OK", "OVERRIDE"));
  EXPECT_TRUE(e.GetString("XRD_TEST_STR_OK", val));
  EXPECT_EQ(val, "OK");

  // get by default value
  EXPECT_FALSE(e.GetDefaultStringValue("DoesNotExist", val));
  EXPECT_EQ(val, "OK"); // unchanged
}


TEST_F(XrdClEnvTest, GetPtr_Success) {
  EXPECT_TRUE(e.PutPtr("gStream", this));
  void *ptr = nullptr;
  EXPECT_TRUE(e.GetPtr("gStream", ptr));
  EXPECT_EQ(ptr, this);
}

TEST_F(XrdClEnvTest, GetPtr_Failure_Unset) {
  void *ptr = nullptr;
  EXPECT_FALSE(e.GetPtr("XRD_TEST_INVALID", ptr));
  EXPECT_EQ(ptr, nullptr);
}
