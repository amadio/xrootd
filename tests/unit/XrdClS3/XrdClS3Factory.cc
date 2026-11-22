/******************************************************************************/
/* Copyright (C) 2025, Pelican Project, Morgridge Institute for Research      */
/*                                                                            */
/* This file is part of the XrdClS3 client plugin for XRootD.                 */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* The copyright holder's institutional names and contributor's names may not */
/* be used to endorse or promote products derived from this software without  */
/* specific prior written permission of the institution or contributor.       */
/******************************************************************************/

#include "gtest/gtest.h"
#include "XrdClS3/XrdClS3Factory.hh"
#include "XrdClS3/XrdClS3File.hh"

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XProtocol/XProtocol.hh>

#include <XrdCl/XrdClStatus.hh>

#include <filesystem>
#include <map>

#include <fcntl.h>

using namespace XrdClS3;

class FactoryCredentialsTest : public testing::Test {
public:

void SetUp() override {
    auto temp_dir_path = std::filesystem::temp_directory_path() / "factory_creds.XXXXXX";
    m_tempdir = temp_dir_path.string();
    ASSERT_TRUE(mkdtemp(m_tempdir.data()) != NULL) << "Failed to create temporary directory for credentials";
};

void TearDown() override {
    if (!m_tempdir.empty()) {
        std::filesystem::remove_all(m_tempdir);
    }
};

std::string GetTempLocation(const std::string &name) const {
    return (std::filesystem::path(m_tempdir) / name).string();
}

bool WriteFile(const std::string &location, const std::string &contents) {
    std::filesystem::path path(m_tempdir);
    path /= location;
    int fd = open(path.c_str(), O_CREAT|O_TRUNC|O_WRONLY, 0600);
    if (fd == -1) return false;

	auto ptr = &contents[0];
	ssize_t nwrite;
	auto nleft = contents.size();

	while (nleft > 0) {
	REISSUE_WRITE:
		nwrite = write(fd, ptr, nleft);
		if (nwrite < 0) {
			if (errno == EINTR) {
				goto REISSUE_WRITE;
			}
			close(fd);
			return false;
		}

		nleft -= nwrite;
		ptr += nwrite;
	}

	close(fd);
	return true;
}

bool WriteCreds(const std::string &access_key_location, const std::string &access_key, const std::string &secret_key_location, const std::string &secret_key)
{
    if (!WriteFile(access_key_location, access_key)) return false;

    return WriteFile(secret_key_location, secret_key);
}

private:
    std::string m_tempdir;
};

TEST(FactoryTest, ExtractHostnameRemovesSchemeUserPortPathAndQuery) {
    std::string_view url = "https://bucket-name.s3.amazonaws.com/path/to/object";
    std::string_view hostname = Factory::ExtractHostname(url);
    ASSERT_EQ(hostname, "bucket-name.s3.amazonaws.com");

    url = "https://s3.amazonaws.com";
    hostname = Factory::ExtractHostname(url);
    ASSERT_EQ(hostname, "s3.amazonaws.com");

    url = "s3://p0@localhost:55708/";
    hostname = Factory::ExtractHostname(url);
    ASSERT_EQ(hostname, "localhost");

    url = "https://user:foo@s3.amazonaws.com:9443/";
    hostname = Factory::ExtractHostname(url);
    ASSERT_EQ(hostname, "s3.amazonaws.com");

    url = "https://user:foo@s3.amazonaws.com:9443/foo/bar";
    hostname = Factory::ExtractHostname(url);
    ASSERT_EQ(hostname, "s3.amazonaws.com");

    url = "s3://s3.amazonaws.com?foo=bar";
    hostname = Factory::ExtractHostname(url);
    ASSERT_STREQ(std::string(hostname).c_str(), "s3.amazonaws.com");

    url = "s3.amazonaws.com?foo=bar";
    hostname = Factory::ExtractHostname(url);
    ASSERT_EQ(hostname, "");
}

TEST(FactoryTest, GenerateHttpUrlBuildsUrlsForVirtualAndPathStyles) {
    Factory::SetEndpoint("s3.amazonaws.com");
    Factory::SetRegion("us-east-1");
    Factory::SetUrlStyle("virtual");

    std::string s3_url = "s3://bucket-name/path/to/object";
    std::string https_url;
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://bucket-name.us-east-1.s3.amazonaws.com/path/to/object");

    Factory::SetEndpoint("");
    s3_url = "s3://p0@localhost:55708/bucket-name/object";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://bucket-name.us-east-1.localhost:55708/object");

    Factory::SetEndpoint("s3.amazonaws.com");
    s3_url = "s3://bucket-name/path/to/object?versionId=12345";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://bucket-name.us-east-1.s3.amazonaws.com/path/to/object?versionId=12345");

    s3_url = "s3://bucket-name.us-east-1.s3.amazonaws.com/path/to/object";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://bucket-name.us-east-1.s3.amazonaws.com/path/to/object");
    
    s3_url = "s3://s3.amazonaws.com/bucket-name/path/to/object";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://bucket-name.us-east-1.s3.amazonaws.com/path/to/object");

    Factory::SetEndpoint("");
    s3_url = "s3://s3.amazonaws.com/bucket-name/path";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://bucket-name.us-east-1.s3.amazonaws.com/path");

    Factory::SetUrlStyle("path");
    s3_url = "s3://s3.amazonaws.com/bucket-name/path";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://us-east-1.s3.amazonaws.com/bucket-name/path");

    Factory::SetRegion("");
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://s3.amazonaws.com/bucket-name/path");

    std::string obj;
    s3_url = "s3://localhost:9000/mybucket";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, &obj).IsOK());
    ASSERT_EQ(https_url, "https://localhost:9000/mybucket");
    ASSERT_TRUE(obj.empty());

    s3_url = "s3://localhost:9000/mybucket/";
    ASSERT_TRUE(Factory::GenerateHttpUrl(s3_url, https_url, &obj).IsOK());
    ASSERT_EQ(https_url, "https://localhost:9000/mybucket");
    ASSERT_TRUE(obj.empty());

    s3_url = "s3://localhost:9000/";
    ASSERT_FALSE(Factory::GenerateHttpUrl(s3_url, https_url, &obj).IsOK());
}

TEST(FactoryTest, GetBucketFromHttpsUrlFindsTheBucketInBothUrlStyles) {
    std::string bucket_name;
    Factory::SetEndpoint("s3.amazonaws.com");
    Factory::SetRegion("us-east-1");
    Factory::SetUrlStyle("virtual");

    bucket_name = Factory::GetBucketFromHttpsUrl("https://bucket-name.us-east-1.s3.amazonaws.com/path/to/object");
    ASSERT_EQ(bucket_name, "bucket-name");

    bucket_name = Factory::GetBucketFromHttpsUrl("https://bucket-name.s3.amazonaws.com/path/to/object");
    ASSERT_EQ(bucket_name, "bucket-name");

    Factory::SetRegion("");
    bucket_name = Factory::GetBucketFromHttpsUrl("https://bucket-name.s3.amazonaws.com/path/to/object");
    ASSERT_EQ(bucket_name, "bucket-name");

    Factory::SetUrlStyle("path");
    bucket_name = Factory::GetBucketFromHttpsUrl("https://s3.amazonaws.com/bucket-name/path/to/object");
    ASSERT_EQ(bucket_name, "bucket-name");
}

TEST_F(FactoryCredentialsTest, GetCredentialsForBucketReadsAndCachesTheKeyFiles) {

    WriteCreds("access_key.txt", "my_access_key",
               "secret_key.txt", "my_secret_key");
    Factory::SetDefaultCredentials(GetTempLocation("access_key.txt"), GetTempLocation("secret_key.txt"));
    std::string err_msg;
    auto [access, secret, ok] = Factory::GetCredentialsForBucket("bucket-name", err_msg);
    ASSERT_TRUE(ok) << "Failure to get credentials for bucket: " << err_msg;
    ASSERT_EQ(access, "my_access_key");
    ASSERT_EQ(secret, "my_secret_key");

    WriteCreds("access_key.bucket.txt", "my_access_key.bucket",
               "secret_key.bucket.txt", "my_secret_key.bucket");

    Factory::SetBucketCredentials("bucket-name", GetTempLocation("access_key.bucket.txt"), GetTempLocation("secret_key.bucket.txt"));

    std::tie(access, secret, ok) = Factory::GetCredentialsForBucket("bucket-name2", err_msg);
    ASSERT_TRUE(ok);
    ASSERT_EQ(access, "my_access_key");
    ASSERT_EQ(secret, "my_secret_key");

    std::tie(access, secret, ok) = Factory::GetCredentialsForBucket("bucket-name", err_msg);
    ASSERT_TRUE(ok);
    ASSERT_EQ(access, "my_access_key"); // Due to caching, should get the old value not the new one.
    ASSERT_EQ(secret, "my_secret_key");

    Factory::SetBucketCredentials("bucket-name3", GetTempLocation("access_key.bucket.txt"), GetTempLocation("secret_key.bucket.txt"));
    std::tie(access, secret, ok) = Factory::GetCredentialsForBucket("bucket-name3", err_msg);
    ASSERT_TRUE(ok);
    ASSERT_EQ(access, "my_access_key.bucket");
    ASSERT_EQ(secret, "my_secret_key.bucket");

    Factory::SetBucketCredentials("bucket-name4", GetTempLocation("access_key.bucket2.txt"), GetTempLocation("secret_key.bucket2.txt"));

    std::tie(access, secret, ok) = Factory::GetCredentialsForBucket("bucket-name4", err_msg);
    ASSERT_FALSE(ok);
}

TEST_F(FactoryCredentialsTest, GenerateV4SignatureMatchesTheAwsExamples) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("s3.amazonaws.com");
    Factory::SetRegion("us-east-1");
    Factory::SetUrlStyle("virtual");

    WriteCreds("access_key.txt", "AKIAIOSFODNN7EXAMPLE", "secret_key.txt", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    Factory::SetDefaultCredentials(GetTempLocation("access_key.txt"), GetTempLocation("secret_key.txt"));

    // Examples come from https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Range", "bytes=0-9"},
        {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"x-amz-date", "20130524T000000Z"}
    };
    std::string auth_token, err_msg;
    ASSERT_TRUE(Factory::GenerateV4Signature("https://examplebucket.s3.amazonaws.com/test.txt", "GET", headers, auth_token, err_msg));
    ASSERT_EQ(err_msg, "");
    ASSERT_EQ(auth_token, "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");

    headers = {
        {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"x-amz-date", "20130524T000000Z"}
    };
    ASSERT_TRUE(Factory::GenerateV4Signature("https://examplebucket.s3.amazonaws.com?lifecycle", "GET", headers, auth_token, err_msg));
    ASSERT_EQ(err_msg, "");
    ASSERT_EQ(auth_token, "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,SignedHeaders=host;x-amz-content-sha256;x-amz-date,Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");

    ASSERT_TRUE(Factory::GenerateV4Signature("https://examplebucket.s3.amazonaws.com?max-keys=2&prefix=J", "GET", headers, auth_token, err_msg));
    ASSERT_EQ(err_msg, "");
    ASSERT_EQ(auth_token, "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,SignedHeaders=host;x-amz-content-sha256;x-amz-date,Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");

    headers = {
        {"Date", "Fri, 24 May 2013 00:00:00 GMT"},
        {"x-amz-date", "20130524T000000Z"},
        {"x-amz-storage-class", "REDUCED_REDUNDANCY"},
        {"x-amz-content-sha256", "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"}
    };
    ASSERT_TRUE(Factory::GenerateV4Signature("https://examplebucket.s3.amazonaws.com/test$file.text", "PUT", headers, auth_token, err_msg));
    ASSERT_EQ(err_msg, "");
    ASSERT_EQ(auth_token, "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class,Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
}

TEST(FactoryTest, PathEncodeReturnsTheEncodedPathWithoutQuery) {
    ASSERT_EQ(Factory::PathEncode("https://examplebucket.s3.amazonaws.com/test.txt"), "/test.txt");
    ASSERT_EQ(Factory::PathEncode("https://examplebucket.s3.amazonaws.com/test//sub.txt"), "/test//sub.txt");
    ASSERT_EQ(Factory::PathEncode("https://examplebucket.s3.amazonaws.com/test.txt?foo=bar"), "/test.txt");
    ASSERT_EQ(Factory::PathEncode("https://examplebucket.s3.amazonaws.com?lifecycle"), "/");
    ASSERT_EQ(Factory::PathEncode("https://examplebucket.s3.amazonaws.com/test$file.text"), "/test\%24file.text");
}

TEST(FactoryTest, CleanObjectNameRemovesAllAuthzParameters) {
    ASSERT_EQ(Factory::CleanObjectName("test.txt"), "test.txt");
    ASSERT_EQ(Factory::CleanObjectName("test//sub.txt"), "test//sub.txt");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?foo=bar"), "test.txt?foo=bar");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?authz="), "test.txt");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?authz=foo"), "test.txt");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?&authz=foo"), "test.txt");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?foo=bar&authz=foo"), "test.txt?foo=bar");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?foo=bar&authz=foo&authz=bar"), "test.txt?foo=bar");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?foo=bar&authz=foo&foo=bar"), "test.txt?foo=bar&foo=bar");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?foo=bar&authz=foo&foo=bar"), "test.txt?foo=bar&foo=bar");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?&foo=bar&authz=foo&foo=bar"), "test.txt?foo=bar&foo=bar");
    ASSERT_EQ(Factory::CleanObjectName("test.txt?&foo=bar&oss.asize&authz=foo&foo=bar"), "test.txt?foo=bar&oss.asize&foo=bar");
}

// The signature of a query, with the date of the examples of AWS. A failure
// fails the test, thus two failures do not compare as equal signatures.
static std::string SignQuery(const std::string &query) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"x-amz-date", "20130524T000000Z"}
    };
    std::string auth_token, err_msg;
    if (!Factory::GenerateV4Signature("https://examplebucket.s3.amazonaws.com?" + query, "GET", headers, auth_token, err_msg)) {
        ADD_FAILURE() << "no signature of the query " << query << ": " << err_msg;
        return {};
    }
    return auth_token;
}

TEST_F(FactoryCredentialsTest, GenerateV4SignatureOfAnEncodedValueIsTheOneOfTheValue) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("s3.amazonaws.com");
    Factory::SetRegion("us-east-1");
    Factory::SetUrlStyle("virtual");
    WriteCreds("access_key.txt", "AKIAIOSFODNN7EXAMPLE", "secret_key.txt", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    Factory::SetDefaultCredentials(GetTempLocation("access_key.txt"), GetTempLocation("secret_key.txt"));

    ASSERT_EQ(SignQuery("prefix=a%2Fb"), SignQuery("prefix=a/b"));
}

// A service reads a '+' in a query as a space
TEST_F(FactoryCredentialsTest, GenerateV4SignatureOfAPlusIsTheOneOfASpace) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("s3.amazonaws.com");
    Factory::SetRegion("us-east-1");
    Factory::SetUrlStyle("virtual");
    WriteCreds("access_key.txt", "AKIAIOSFODNN7EXAMPLE", "secret_key.txt", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    Factory::SetDefaultCredentials(GetTempLocation("access_key.txt"), GetTempLocation("secret_key.txt"));

    ASSERT_EQ(SignQuery("prefix=a+b"), SignQuery("prefix=a%20b"));
}

TEST(FactoryTest, GenerateHttpUrlRejectsAUrlWhichIsNotS3) {
    std::string https_url;
    auto st = Factory::GenerateHttpUrl("https://localhost/bucket/object", https_url, nullptr);
    ASSERT_FALSE(st.IsOK());
    ASSERT_FALSE(st.GetErrorMessage().empty());
}

TEST(FactoryTest, GenerateHttpUrlRejectsAUrlWithoutABucket) {
    Factory::SetEndpoint("");
    Factory::SetRegion("");
    Factory::SetUrlStyle("path");
    std::string https_url;
    ASSERT_FALSE(Factory::GenerateHttpUrl("s3://localhost:9000", https_url, nullptr).IsOK());
    auto st = Factory::GenerateHttpUrl("s3://localhost:9000/?versionId=1", https_url, nullptr);
    ASSERT_FALSE(st.IsOK());
    ASSERT_FALSE(st.GetErrorMessage().empty());
}

TEST(FactoryTest, GenerateHttpUrlKeepsOnlyTheHostOfTheAuthorityForThePath) {
    Factory::SetEndpoint("");
    Factory::SetRegion("");
    Factory::SetUrlStyle("path");
    std::string https_url, obj;
    ASSERT_TRUE(Factory::GenerateHttpUrl("s3://user@localhost:9000/bucket/dir/object", https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://localhost:9000/bucket/dir/object");

    ASSERT_TRUE(Factory::GenerateHttpUrl("s3://localhost:9000/bucket/dir/object", https_url, &obj).IsOK());
    ASSERT_EQ(https_url, "https://localhost:9000/bucket");
    ASSERT_EQ(obj, "dir/object");
}

TEST(FactoryTest, GenerateHttpUrlFindsTheBucketInAVirtualHostAuthority) {
    Factory::SetEndpoint("s3.example.org");
    Factory::SetUrlStyle("virtual");
    std::string https_url;

    // the authority has the region, or not
    Factory::SetRegion("region-1");
    ASSERT_TRUE(Factory::GenerateHttpUrl("s3://mybucket.region-1.s3.example.org/key", https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://mybucket.region-1.s3.example.org/key");
    ASSERT_TRUE(Factory::GenerateHttpUrl("s3://mybucket.s3.example.org/key", https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://mybucket.region-1.s3.example.org/key");
    Factory::SetRegion("");
}

// Without a region, the host of a virtual-host URL is <bucket>.<endpoint>
TEST(FactoryTest, GenerateHttpUrlBuildsAVirtualHostUrlWithoutARegion) {
    Factory::SetEndpoint("s3.example.org");
    Factory::SetRegion("");
    Factory::SetUrlStyle("virtual");
    std::string https_url;
    ASSERT_TRUE(Factory::GenerateHttpUrl("s3://mybucket.s3.example.org/key", https_url, nullptr).IsOK());
    ASSERT_EQ(https_url, "https://mybucket.s3.example.org/key");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl(https_url), "mybucket");
    Factory::SetUrlStyle("path");
}

TEST(FactoryTest, GenerateHttpUrlRejectsAnUnknownUrlStyle) {
    Factory::SetEndpoint("");
    Factory::SetRegion("");
    Factory::SetUrlStyle("other");
    std::string https_url;
    auto st = Factory::GenerateHttpUrl("s3://localhost:9000/bucket/object", https_url, nullptr);
    ASSERT_FALSE(st.IsOK());
    ASSERT_EQ(st.code, XrdCl::errConfig);
    ASSERT_FALSE(st.GetErrorMessage().empty());
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("https://localhost:9000/bucket/object"), "");
    Factory::SetUrlStyle("path");
}

TEST(FactoryTest, GetBucketFromHttpsUrlFindsNoBucketInAnUnknownHost) {
    Factory::SetEndpoint("s3.example.org");
    Factory::SetRegion("region-1");
    Factory::SetUrlStyle("virtual");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("https://mybucket.s3.example.org/key"), "mybucket");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("https://mybucket.other.org/key"), "");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("not a url"), "");
    Factory::SetRegion("");
    Factory::SetUrlStyle("path");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("https://localhost:9000/bucket"), "bucket");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("https://localhost:9000"), "");
    ASSERT_EQ(Factory::GetBucketFromHttpsUrl("localhost:9000/bucket"), "");
}

TEST(FactoryTest, PathEncodeOfAUrlWithOnlyAQueryIsTheRoot) {
    ASSERT_EQ(Factory::PathEncode("https://localhost:9000?list-type=2"), "/");
    ASSERT_EQ(Factory::PathEncode("localhost"), "");
}

TEST(FactoryTest, TrimViewOfOnlySpacesIsEmpty) {
    ASSERT_EQ(Factory::TrimView(" \t\n "), "");
    ASSERT_EQ(Factory::TrimView(" key \n"), "key");
}

// A byte above 127 is negative in a signed char, which isspace must not get
TEST(FactoryTest, TrimViewKeepsANonAsciiByte) {
    ASSERT_EQ(Factory::TrimView(" \xff "), "\xff");
}

TEST_F(FactoryCredentialsTest, GetCredentialsForBucketOfAPublicBucketAreEmpty) {
    Factory::ResetCredCache();
    Factory::SetBucketCredentials("public-bucket", "", "");
    std::string err_msg;
    auto [access, secret, ok] = Factory::GetCredentialsForBucket("public-bucket", err_msg);
    ASSERT_TRUE(ok) << err_msg;
    ASSERT_EQ(access, "");
    ASSERT_EQ(secret, "");
}

TEST_F(FactoryCredentialsTest, GetCredentialsForBucketWithOnlyOneKeyFails) {
    Factory::ResetCredCache();
    WriteFile("access_key.half.txt", "my_access_key");
    Factory::SetBucketCredentials("half-bucket", GetTempLocation("access_key.half.txt"), "");
    std::string err_msg;
    auto [access, secret, ok] = Factory::GetCredentialsForBucket("half-bucket", err_msg);
    ASSERT_FALSE(ok);
    ASSERT_FALSE(err_msg.empty());
}

TEST_F(FactoryCredentialsTest, GetCredentialsForBucketWithEmptyKeyFilesFails) {
    Factory::ResetCredCache();
    WriteCreds("access_key.empty.txt", " \n", "secret_key.empty.txt", "my_secret_key");
    Factory::SetBucketCredentials("empty-bucket", GetTempLocation("access_key.empty.txt"), GetTempLocation("secret_key.empty.txt"));
    std::string err_msg;
    auto [access, secret, ok] = Factory::GetCredentialsForBucket("empty-bucket", err_msg);
    ASSERT_FALSE(ok);
    ASSERT_FALSE(err_msg.empty());
}

TEST_F(FactoryCredentialsTest, GetCredentialsForBucketWithoutTheSecretKeyFileFails) {
    Factory::ResetCredCache();
    WriteFile("access_key.nosecret.txt", "my_access_key");
    Factory::SetBucketCredentials("nosecret-bucket", GetTempLocation("access_key.nosecret.txt"), GetTempLocation("missing.txt"));
    std::string err_msg;
    auto [access, secret, ok] = Factory::GetCredentialsForBucket("nosecret-bucket", err_msg);
    ASSERT_FALSE(ok);
    ASSERT_FALSE(err_msg.empty());
}

TEST_F(FactoryCredentialsTest, GenerateV4SignatureOfAPublicBucketIsEmpty) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("");
    Factory::SetRegion("");
    Factory::SetUrlStyle("path");
    Factory::SetBucketCredentials("signature-public", "", "");
    std::vector<std::pair<std::string, std::string>> headers;
    std::string auth_token = "unset", err_msg;
    ASSERT_TRUE(Factory::GenerateV4Signature("https://localhost:9000/signature-public/key", "GET", headers, auth_token, err_msg));
    ASSERT_EQ(auth_token, "");
    ASSERT_TRUE(headers.empty());
}

TEST_F(FactoryCredentialsTest, GenerateV4SignatureAddsTheHostTheDateAndThePayloadHash) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("");
    Factory::SetRegion("us-east-1");
    Factory::SetUrlStyle("path");
    WriteCreds("access_key.sig.txt", "AKIAIOSFODNN7EXAMPLE", "secret_key.sig.txt", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    Factory::SetBucketCredentials("signature-bucket", GetTempLocation("access_key.sig.txt"), GetTempLocation("secret_key.sig.txt"));
    std::vector<std::pair<std::string, std::string>> headers;
    std::string auth_token, err_msg;
    ASSERT_TRUE(Factory::GenerateV4Signature("https://localhost:9000/signature-bucket/key", "GET", headers, auth_token, err_msg)) << err_msg;
    std::map<std::string, std::string> added(headers.begin(), headers.end());
    ASSERT_EQ(added["Host"], "localhost");
    ASSERT_EQ(added["X-Amz-Content-Sha256"], "UNSIGNED-PAYLOAD");
    ASSERT_EQ(added["X-Amz-Date"].size(), 16u);
    ASSERT_EQ(added["X-Amz-Date"][8], 'T');
    ASSERT_EQ(auth_token.rfind("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/" + added["X-Amz-Date"].substr(0, 8) + "/us-east-1/s3/aws4_request,", 0), 0u);
    Factory::SetRegion("");
}

TEST_F(FactoryCredentialsTest, GenerateV4SignatureRejectsAnInvalidDate) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("");
    Factory::SetRegion("");
    Factory::SetUrlStyle("path");
    WriteCreds("access_key.date.txt", "my_access_key", "secret_key.date.txt", "my_secret_key");
    Factory::SetBucketCredentials("date-bucket", GetTempLocation("access_key.date.txt"), GetTempLocation("secret_key.date.txt"));
    std::vector<std::pair<std::string, std::string>> headers = {{"x-amz-date", "2013-05-24T00:00:00Z"}};
    std::string auth_token, err_msg;
    ASSERT_FALSE(Factory::GenerateV4Signature("https://localhost:9000/date-bucket/key", "GET", headers, auth_token, err_msg));
    ASSERT_FALSE(err_msg.empty());
}

TEST_F(FactoryCredentialsTest, GenerateV4SignatureFailsWithoutTheKeyFiles) {
    Factory::ResetCredCache();
    Factory::SetEndpoint("");
    Factory::SetRegion("");
    Factory::SetUrlStyle("path");
    Factory::SetBucketCredentials("nokeys-bucket", GetTempLocation("missing_access.txt"), GetTempLocation("missing_secret.txt"));
    std::vector<std::pair<std::string, std::string>> headers;
    std::string auth_token, err_msg;
    ASSERT_FALSE(Factory::GenerateV4Signature("https://localhost:9000/nokeys-bucket/key", "GET", headers, auth_token, err_msg));
    ASSERT_FALSE(err_msg.empty());
}
