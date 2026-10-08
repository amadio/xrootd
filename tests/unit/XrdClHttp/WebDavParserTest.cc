/******************************************************************************/
/* Copyright (C) 2026, XRootD Collaboration                                  */
/*                                                                            */
/* This file is part of the XrdClHttp client plugin for XRootD.               */
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

#include "XrdClHttp/XrdClHttpUtil.hh"
#include "XrdClHttp/XrdClHttpWebDav.hh"

#include <gtest/gtest.h>
#include <tinyxml.h>

namespace {

bool ParseProperties(const char *xml, XrdClHttp::WebDavProperties &properties)
{
    TiXmlDocument document;
    document.Parse(xml);
    return !document.Error() &&
        XrdClHttp::ParseWebDavProperties(document.RootElement(), properties);
}

bool ParseResponseProperties(const char *xml,
                             XrdClHttp::WebDavProperties &properties)
{
    TiXmlDocument document;
    document.Parse(xml);
    return !document.Error() && XrdClHttp::ParseWebDavResponseProperties(
        document.RootElement(), properties);
}

bool ParseResponseQuota(const char *xml, XrdClHttp::WebDavQuota &quota)
{
    TiXmlDocument document;
    document.Parse(xml);
    return !document.Error() && XrdClHttp::ParseWebDavResponseQuota(
        document.RootElement(), quota);
}

}

TEST(WebDavParser, ParsesWhitespaceSeparatedAllowMethods)
{
    XrdClHttp::HeaderParser parser;
    EXPECT_TRUE(parser.Parse("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(parser.Parse("Allow: MKCOL, PROPFIND, PUT\r\n"));
    EXPECT_TRUE(parser.Parse("\r\n"));

    EXPECT_TRUE(parser.GetAllowedVerbs().IsSet(
        XrdClHttp::VerbsCache::HttpVerb::kPROPFIND));
}

TEST(WebDavParser, MatchesLocalNamesWithArbitraryPrefixes)
{
    TiXmlDocument document;
    document.Parse("<ns0:collection xmlns:ns0=\"DAV:\"/>");

    EXPECT_TRUE(XrdClHttp::WebDavElementNameEquals(
        document.RootElement(), "collection"));
    EXPECT_FALSE(XrdClHttp::WebDavElementNameEquals(
        document.RootElement(), "resourcetype"));
}

TEST(WebDavParser, AllowsDirectoryWithEmptyContentLength)
{
    XrdClHttp::WebDavProperties properties;
    ASSERT_TRUE(ParseProperties(
        "<d:prop xmlns:d=\"DAV:\">"
        "<d:getcontentlength/>"
        "<d:resourcetype><d:collection/></d:resourcetype>"
        "</d:prop>", properties));

    EXPECT_TRUE(properties.m_is_dir);
    EXPECT_EQ(properties.m_size, 0);
}

TEST(WebDavParser, RequiresContentLengthForRegularResources)
{
    XrdClHttp::WebDavProperties missing;
    EXPECT_FALSE(ParseProperties(
        "<d:prop xmlns:d=\"DAV:\"><d:resourcetype/></d:prop>", missing));

    XrdClHttp::WebDavProperties empty;
    EXPECT_FALSE(ParseProperties(
        "<d:prop xmlns:d=\"DAV:\"><d:getcontentlength/></d:prop>", empty));
}

TEST(WebDavParser, ParsesValidRegularResourceSize)
{
    XrdClHttp::WebDavProperties properties;
    ASSERT_TRUE(ParseProperties(
        "<d:prop xmlns:d=\"DAV:\">"
        "<d:getcontentlength> 123 </d:getcontentlength>"
        "<d:resourcetype/>"
        "</d:prop>", properties));

    EXPECT_FALSE(properties.m_is_dir);
    EXPECT_EQ(properties.m_size, 123);
}

TEST(WebDavParser, RejectsInvalidContentLength)
{
    for (auto value : {"-1", "12 bytes", "9223372036854775808"}) {
        XrdClHttp::WebDavProperties properties;
        std::string xml = "<d:prop xmlns:d=\"DAV:\"><d:getcontentlength>";
        xml += value;
        xml += "</d:getcontentlength></d:prop>";
        EXPECT_FALSE(ParseProperties(xml.c_str(), properties)) << value;
    }
}

TEST(WebDavParser, IgnoresUnsuccessfulPropstatEntries)
{
    XrdClHttp::WebDavProperties properties;
    ASSERT_TRUE(ParseResponseProperties(
        "<d:response xmlns:d=\"DAV:\">"
        "<d:propstat>"
        "<d:status>HTTP/1.1 200 OK</d:status>"
        "<d:prop><d:getcontentlength>123</d:getcontentlength></d:prop>"
        "</d:propstat>"
        "<d:propstat>"
        "<d:status>HTTP/1.1 404 Not Found</d:status><d:prop/>"
        "</d:propstat>"
        "</d:response>", properties));

    EXPECT_EQ(properties.m_size, 123);
}

TEST(WebDavParser, RequiresSuccessfulPropstatEntry)
{
    XrdClHttp::WebDavProperties properties;
    EXPECT_FALSE(ParseResponseProperties(
        "<d:response xmlns:d=\"DAV:\">"
        "<d:propstat>"
        "<d:status>HTTP/1.1 404 Not Found</d:status><d:prop/>"
        "</d:propstat>"
        "</d:response>", properties));
}

TEST(WebDavParser, ParsesQuotaWithArbitraryPrefix)
{
    XrdClHttp::WebDavQuota quota;
    ASSERT_TRUE(ParseResponseQuota(
        "<x:response xmlns:x=\"DAV:\"><x:propstat>"
        "<x:status>HTTP/1.1 200 OK</x:status><x:prop>"
        "<x:quota-used-bytes>17</x:quota-used-bytes>"
        "<x:quota-available-bytes>83</x:quota-available-bytes>"
        "</x:prop></x:propstat></x:response>", quota));
    EXPECT_EQ(quota.m_used, 17);
    EXPECT_EQ(quota.m_available, 83);
}

TEST(WebDavParser, RequiresBothQuotaProperties)
{
    XrdClHttp::WebDavQuota quota;
    EXPECT_FALSE(ParseResponseQuota(
        "<d:response xmlns:d=\"DAV:\"><d:propstat>"
        "<d:status>HTTP/1.1 200 OK</d:status><d:prop>"
        "<d:quota-used-bytes>17</d:quota-used-bytes>"
        "</d:prop></d:propstat></d:response>", quota));
}

TEST(WebDavParser, TrimsEmptyAndWhitespaceOnlyValuesSafely)
{
    for (const auto value : {"", " ", "\t\r\n", "  \t  "})
        EXPECT_TRUE(XrdClHttp::trim_view(value).empty());
    EXPECT_EQ(XrdClHttp::trim_view(" \tvalue\r\n "), "value");
}

TEST(WebDavParser, CombinesSuccessfulMetadataGroups)
{
    const std::string size =
        "<d:propstat><d:status>HTTP/1.1 200 OK</d:status>"
        "<d:prop><d:getcontentlength>123</d:getcontentlength></d:prop>"
        "</d:propstat>";
    const std::string metadata =
        "<d:propstat><d:status>HTTP/1.1 200 OK</d:status><d:prop>"
        "<d:resourcetype><d:collection/></d:resourcetype>"
        "<d:executable>T</d:executable></d:prop></d:propstat>";
    for (const auto &groups : {size + metadata, metadata + size}) {
        XrdClHttp::WebDavProperties properties;
        const auto xml = "<d:response xmlns:d=\"DAV:\">" + groups +
            "<d:propstat><d:status>HTTP/1.1 404 Not Found</d:status>"
            "<d:prop><d:getcontentlength>999</d:getcontentlength></d:prop>"
            "</d:propstat></d:response>";
        ASSERT_TRUE(ParseResponseProperties(xml.c_str(), properties));
        EXPECT_EQ(properties.m_size, 123);
        EXPECT_TRUE(properties.m_is_dir);
        EXPECT_TRUE(properties.m_is_executable);
    }
}

TEST(WebDavParser, CombinesRegularFileMetadataBeforeValidatingSize)
{
    XrdClHttp::WebDavProperties properties;
    ASSERT_TRUE(ParseResponseProperties(
        "<d:response xmlns:d=\"DAV:\">"
        "<d:propstat><d:status>HTTP/1.1 200 OK</d:status>"
        "<d:prop><d:resourcetype/><d:executable>T</d:executable></d:prop>"
        "</d:propstat><d:propstat><d:status>HTTP/1.1 200 OK</d:status>"
        "<d:prop><d:getcontentlength>42</d:getcontentlength></d:prop>"
        "</d:propstat></d:response>", properties));
    EXPECT_EQ(properties.m_size, 42);
    EXPECT_FALSE(properties.m_is_dir);
    EXPECT_TRUE(properties.m_is_executable);
}

TEST(WebDavParser, RejectsWhitespaceOnlyStatusAndContentLength)
{
    XrdClHttp::WebDavProperties properties;
    EXPECT_FALSE(ParseResponseProperties(
        "<d:response xmlns:d=\"DAV:\"><d:propstat>"
        "<d:status>  \t </d:status>"
        "<d:prop><d:getcontentlength>42</d:getcontentlength></d:prop>"
        "</d:propstat></d:response>", properties));
    EXPECT_FALSE(ParseProperties(
        "<d:prop xmlns:d=\"DAV:\"><d:getcontentlength> \t </d:getcontentlength>"
        "</d:prop>", properties));
}

TEST(WebDavParser, CombinesQuotaGroupsAndIgnoresUnrelatedProperties)
{
    const std::string available =
        "<d:propstat><d:status>HTTP/1.1 200 OK</d:status><d:prop>"
        "<d:quota-available-bytes>83</d:quota-available-bytes>"
        "<d:displayname>storage</d:displayname></d:prop></d:propstat>";
    const std::string used =
        "<d:propstat><d:status>HTTP/1.1 200 OK</d:status><d:prop>"
        "<d:quota-used-bytes>17</d:quota-used-bytes></d:prop></d:propstat>";
    for (const auto &groups : {available + used, used + available}) {
        const auto xml = "<d:response xmlns:d=\"DAV:\">" + groups +
            "<d:propstat><d:status>HTTP/1.1 404 Not Found</d:status><d:prop>"
            "<d:quota-used-bytes>999</d:quota-used-bytes></d:prop>"
            "</d:propstat></d:response>";
        XrdClHttp::WebDavQuota quota;
        ASSERT_TRUE(ParseResponseQuota(xml.c_str(), quota));
        EXPECT_EQ(quota.m_available, 83);
        EXPECT_EQ(quota.m_used, 17);
    }
}

TEST(WebDavParser, RejectsWhitespaceOnlyQuotaValue)
{
    XrdClHttp::WebDavQuota quota;
    EXPECT_FALSE(ParseResponseQuota(
        "<d:response xmlns:d=\"DAV:\"><d:propstat>"
        "<d:status>HTTP/1.1 200 OK</d:status><d:prop>"
        "<d:quota-used-bytes> \t </d:quota-used-bytes>"
        "<d:quota-available-bytes>83</d:quota-available-bytes>"
        "</d:prop></d:propstat></d:response>", quota));
}

TEST(WebDavParser, RejectsExplicitEmptyQuotaStatus)
{
    for (const auto status : {"", " \t "}) {
        const std::string xml =
            "<d:response xmlns:d=\"DAV:\"><d:propstat><d:status>" +
            std::string(status) + "</d:status><d:prop>"
            "<d:quota-used-bytes>17</d:quota-used-bytes>"
            "<d:quota-available-bytes>83</d:quota-available-bytes>"
            "</d:prop></d:propstat></d:response>";
        XrdClHttp::WebDavQuota quota;
        EXPECT_FALSE(ParseResponseQuota(xml.c_str(), quota));
    }
}
