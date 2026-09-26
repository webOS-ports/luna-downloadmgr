// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// DownloadTask::toJSON() is the wire format for both the downloadStatusQuery
// subscription payload and the rows in the history db, and resumeDownload()
// reads those rows back. Anything that drifts between the two breaks resume
// silently, which is how the e_initialOffsetBytes/e_initialOffset mismatch
// survived: the reader just fell back to a 32-bit field.

#include <gtest/gtest.h>

#include <stdlib.h>
#include <string>

#include "DownloadManager.h"    // DOWNLOADMANAGER_UPDATEINTERVAL / _UPDATENUM
#include "DownloadTask.h"
#include "JUtil.h"

namespace {

// DownloadTask owns a FILE* and a curl handle, so it is deliberately
// non-copyable and non-movable; fill one in place rather than returning by value.
void makeTask(DownloadTask& t)
{
    t.ticket = 4242;
    t.url = "http://example.org/big.iso";
    t.destPath = "/media/internal/downloads/";
    t.destFile = "big.iso";
    t.downloadPrefix = ".";
    t.ownerId = "com.webos.app.test";
    t.connectionName = "wifi";
    t.cookieHeader = "sid=abc";
    t.canHandlePause = true;
    t.autoResume = false;
}

// The keys resumeDownload() looks for. If toJSON() stops emitting one of these,
// resume of an interrupted transfer silently loses precision or fails outright.
TEST(DownloadTaskJson, EmitsEveryKeyResumeReadsBack)
{
    DownloadTask t;
    makeTask(t);
    pbnjson::JValue j = t.toJSON();

    EXPECT_TRUE(j.hasKey("sourceUrl"));
    EXPECT_TRUE(j.hasKey("target"));
    EXPECT_TRUE(j.hasKey("destFile"));
    EXPECT_TRUE(j.hasKey("destPath"));
    EXPECT_TRUE(j.hasKey("destTempPrefix"));
    EXPECT_TRUE(j.hasKey("canHandlePause"));
    EXPECT_TRUE(j.hasKey("autoResume"));
    EXPECT_TRUE(j.hasKey("e_amountTotal"));
    EXPECT_TRUE(j.hasKey("e_amountReceived"));
    EXPECT_TRUE(j.hasKey("e_initialOffsetBytes"));
}

TEST(DownloadTaskJson, SixtyFourBitCountersSurviveTheRoundTrip)
{
    const uint64_t big = 6000000000ull;         // > 2^32
    DownloadTask t;
    makeTask(t);
    t.bytesTotal = big;
    t.bytesCompleted = big - 1000;
    t.initialOffsetBytes = big - 2000;

    pbnjson::JValue j = t.toJSON();

    // the e_ fields carry the full value as a decimal string; strtouq is what
    // resumeDownload() uses to read them back
    EXPECT_EQ(big, strtouq(j["e_amountTotal"].asString().c_str(), 0, 10));
    EXPECT_EQ(big - 1000, strtouq(j["e_amountReceived"].asString().c_str(), 0, 10));
    EXPECT_EQ(big - 2000, strtouq(j["e_initialOffsetBytes"].asString().c_str(), 0, 10));
}

TEST(DownloadTaskJson, TargetIsThePartialFileNotTheFinalOne)
{
    // resumeDownload() opens "target" for append, so it has to be the prefixed
    // temporary name; destPath + destFile is the completed name.
    DownloadTask t;
    makeTask(t);
    pbnjson::JValue j = t.toJSON();

    EXPECT_EQ("/media/internal/downloads/.big.iso", j["target"].asString());
    EXPECT_EQ("big.iso", j["destFile"].asString());
    EXPECT_EQ("/media/internal/downloads/", j["destPath"].asString());
    EXPECT_EQ(".", j["destTempPrefix"].asString());
}

TEST(DownloadTaskJson, QuotesInTheUrlDoNotBreakSerialization)
{
    // The URL is caller-supplied; the serializer has to escape it, and the
    // result has to survive a parse.
    DownloadTask t;
    makeTask(t);
    t.url = "http://example.org/a\"b\\c\nd";

    std::string s = t.toJSONString();
    pbnjson::JValue back = JUtil::parse(s.c_str(), std::string(""));

    ASSERT_FALSE(back.isNull()) << "serialized form did not parse: " << s;
    EXPECT_EQ(t.url, back["sourceUrl"].asString());
}

TEST(DownloadTaskJson, DestToJsonNamesTheFinalFile)
{
    DownloadTask t;
    makeTask(t);
    pbnjson::JValue j = JUtil::parse(t.destToJSON().c_str(), std::string(""));
    ASSERT_FALSE(j.isNull());
    EXPECT_EQ("/media/internal/downloads/big.iso", j["target"].asString());
}

TEST(SetUpdateInterval, StaysWithinItsDocumentedBounds)
{
    DownloadTask t;
    makeTask(t);

    // small transfer: the floor
    t.bytesTotal = 1024;
    t.setUpdateInterval();
    EXPECT_EQ((uint64_t)DOWNLOADMANAGER_UPDATEINTERVAL, t.updateInterval);

    // huge transfer: clamped to the ceiling, never zero (a zero interval would
    // post a subscription update for every single write callback)
    t.bytesTotal = ~(uint64_t)0;
    t.setUpdateInterval();
    EXPECT_GT(t.updateInterval, 0ull);
    EXPECT_LE(t.updateInterval,
              (uint64_t)DOWNLOADMANAGER_UPDATEINTERVAL * DOWNLOADMANAGER_UPDATENUM);
}

TEST(SetMimeType, StripsTrailingCrLfFromTheHeaderValue)
{
    DownloadTask t;
    makeTask(t);
    t.setMimeType("application/octet-stream\r\n");
    EXPECT_EQ("application/octet-stream", t.detectedMIMEType);

    t.setMimeType("\r\n");
    EXPECT_EQ("", t.detectedMIMEType);
}

TEST(RedirectCounter, CountsDownFromTheRfcLimitAndStops)
{
    DownloadTask t;
    makeTask(t);
    EXPECT_EQ(DownloadTask::MAXREDIRECTIONS, t.getRemainingRedCounts());

    for (int i = 0; i < DownloadTask::MAXREDIRECTIONS; ++i)
        t.decreaseRedCounts();

    // completed_dl() cancels the transfer when this hits exactly 0, so the
    // counter must land on 0 rather than skip past it
    EXPECT_EQ(0, t.getRemainingRedCounts());
}

}   // namespace
