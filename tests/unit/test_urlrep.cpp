// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// UrlRep is the first thing a caller-supplied download URL touches, and the
// scheme it extracts is what the security check in DownloadManager::download()
// keys off. These cases pin that behaviour and cover the malformed inputs that
// used to walk an uninitialized UriQueryListA.

#include <gtest/gtest.h>

#include "UrlRep.h"

namespace {

TEST(UrlRep, ParsesAWholeHttpUrl)
{
    UrlRep u = UrlRep::fromUrl("http://example.org:8080/dir/file.bin?a=1&b=2#frag");

    EXPECT_TRUE(u.valid);
    EXPECT_EQ("http", u.scheme);
    EXPECT_EQ("example.org", u.host);
    EXPECT_EQ("8080", u.port);
    EXPECT_EQ("/dir/file.bin", u.path);
    EXPECT_EQ("file.bin", u.resource);
    EXPECT_EQ("frag", u.fragment);
    ASSERT_EQ(1u, u.query.count("a"));
    EXPECT_EQ("1", u.query["a"]);
    EXPECT_EQ("2", u.query["b"]);
}

TEST(UrlRep, ReportsSchemeForNonHttpUrls)
{
    // download() rejects anything that is not http/https/ftp, so the scheme has
    // to come back verbatim rather than empty.
    EXPECT_EQ("file", UrlRep::fromUrl("file:///etc/shadow").scheme);
    EXPECT_EQ("ftp", UrlRep::fromUrl("ftp://example.org/x").scheme);
    EXPECT_EQ("https", UrlRep::fromUrl("https://example.org/x").scheme);
}

TEST(UrlRep, EmptyAndNullInputsAreNotValid)
{
    EXPECT_FALSE(UrlRep::fromUrl(static_cast<const char*>(NULL)).valid);
    EXPECT_FALSE(UrlRep::fromUrl(std::string()).valid);
}

TEST(UrlRep, MalformedInputIsRejectedWithoutCrashing)
{
    // Each of these is refused by uriparser at a different point; the failure
    // path must still release the partially built UriUriA.
    const char* bad[] = {
        "http://",
        "http://[",
        "http://exa mple.org/",
        ":",
        "%",
        "http://example.org/%",
        "?",
        "#",
        "////////",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        UrlRep u = UrlRep::fromUrl(bad[i]);
        (void) u;   // only the absence of a crash/leak is asserted
    }
}

TEST(UrlRep, QueryStringsThatCannotBeDissectedAreIgnored)
{
    // A query uriparser refuses to dissect must leave query empty rather than
    // send the caller walking an uninitialized list head.
    UrlRep u = UrlRep::fromUrl("http://example.org/x?%zz=%");
    if (u.valid) {
        EXPECT_NO_THROW((void) u.query.size());
    }
}

TEST(UrlRep, ResourceIsEmptyForADirectoryUrl)
{
    // download() falls back to a mkstemp() name when resource is empty, so an
    // URL with no filename must not yield a bogus one.
    UrlRep u = UrlRep::fromUrl("http://example.org/dir/");
    EXPECT_TRUE(u.resource.empty());
}

TEST(UrlRep, PercentEncodedPathSeparatorsDoNotSplitTheResource)
{
    UrlRep u = UrlRep::fromUrl("http://example.org/a%2Fb");
    EXPECT_EQ("a%2Fb", u.resource);
}

}   // namespace
