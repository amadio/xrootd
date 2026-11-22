//------------------------------------------------------------------------------
// Tests of the options of the S3 plugin. The first Factory of a process reads
// them from the environment of XrdCl, only once, thus these tests have their
// own executable, which sets the options before it makes the Factory.
//------------------------------------------------------------------------------

#include "gtest/gtest.h"
#include "XrdClS3/XrdClS3Factory.hh"

#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClEnv.hh"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern "C" void *XrdClGetPlugIn(const void *arg);

using namespace XrdClS3;

namespace {

class FactoryConfigTest : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        auto dir = std::filesystem::temp_directory_path() / "factory_config.XXXXXX";
        tempdir = dir.string();
        ASSERT_NE(mkdtemp(tempdir.data()), nullptr);

        Write("access", "default_access\n");
        Write("secret", "default_secret\n");
        Write("access.a", "bucket_a_access");
        Write("secret.a", "bucket_a_secret");

        auto env = XrdCl::DefaultEnv::GetEnv();
        env->PutString("XrdClS3MkdirSentinel", ".sentinel");
        env->PutString("XrdClS3Endpoint", "s3.example.org");
        env->PutString("XrdClS3UrlStyle", "virtual");
        env->PutString("XrdClS3Region", "region-1");
        env->PutString("XrdClS3AccessKeyLocation", Path("access"));
        env->PutString("XrdClS3SecretKeyLocation", Path("secret"));

        // a: keys of its own, b: public, c and e: only one of the two keys,
        // d: no bucket name
        env->PutString("XrdClS3BucketConfigs", "a\n b \nc\nd\ne");
        env->PutString("XrdClS3aBucketName", "bucket-a");
        env->PutString("XrdClS3aAccessKeyLocation", Path("access.a"));
        env->PutString("XrdClS3aSecretKeyLocation", Path("secret.a"));
        env->PutString("XrdClS3bBucketName", "bucket-b");
        env->PutString("XrdClS3cBucketName", "bucket-c");
        env->PutString("XrdClS3cAccessKeyLocation", Path("access.a"));
        env->PutString("XrdClS3eBucketName", "bucket-e");
        env->PutString("XrdClS3eSecretKeyLocation", Path("secret.a"));

        factory = std::make_unique<Factory>();
    }

    static void TearDownTestSuite()
    {
        factory.reset();
        std::filesystem::remove_all(tempdir);
    }

    static std::string Path(const std::string &name)
    {
        return (std::filesystem::path(tempdir) / name).string();
    }

    static void Write(const std::string &name, const std::string &contents)
    {
        std::ofstream(Path(name)) << contents;
    }

    // The keys of a bucket, as "<access>:<secret>", or "error"
    static std::string Keys(const std::string &bucket)
    {
        std::string err_msg;
        auto [access, secret, ok] = Factory::GetCredentialsForBucket(bucket, err_msg);
        return ok ? access + ":" + secret : "error";
    }

    static inline std::string tempdir;
    static inline std::unique_ptr<Factory> factory;
};

TEST_F(FactoryConfigTest, ReadsTheNameOfTheMkdirSentinel)
{
    EXPECT_EQ(Factory::GetMkdirSentinel(), ".sentinel");
}

TEST_F(FactoryConfigTest, ReadsTheUrlStyleTheEndpointAndTheRegion)
{
    std::string https_url;
    auto st = Factory::GenerateHttpUrl("s3://mybucket/key", https_url, nullptr);
    ASSERT_TRUE(st.IsOK()) << st.ToStr();
    EXPECT_EQ(https_url, "https://mybucket.region-1.s3.example.org/key");
}

TEST_F(FactoryConfigTest, UsesTheDefaultKeysForABucketWithoutAConfig)
{
    EXPECT_EQ(Keys("other-bucket"), "default_access:default_secret");
}

TEST_F(FactoryConfigTest, UsesTheKeysOfTheConfigOfABucket)
{
    EXPECT_EQ(Keys("bucket-a"), "bucket_a_access:bucket_a_secret");
}

TEST_F(FactoryConfigTest, TreatsABucketConfigWithoutKeysAsPublic)
{
    EXPECT_EQ(Keys("bucket-b"), ":");
}

TEST_F(FactoryConfigTest, IgnoresABucketConfigWithOnlyOneKey)
{
    EXPECT_EQ(Keys("bucket-c"), "default_access:default_secret");
    EXPECT_EQ(Keys("bucket-e"), "default_access:default_secret");
}

TEST_F(FactoryConfigTest, CreatesTheFileAndTheFileSystemOfAUrl)
{
    std::unique_ptr<XrdCl::FilePlugIn> file(factory->CreateFile("s3://mybucket/key"));
    EXPECT_NE(file, nullptr);
    std::unique_ptr<XrdCl::FileSystemPlugIn> fs(factory->CreateFileSystem("s3://mybucket/"));
    EXPECT_NE(fs, nullptr);
}

TEST_F(FactoryConfigTest, XrdClGetPlugInReturnsAFactory)
{
    std::unique_ptr<Factory> plugin(static_cast<Factory *>(XrdClGetPlugIn(nullptr)));
    EXPECT_NE(plugin, nullptr);
}

}
