// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// The messages a download or upload subscriber gets after the first reply:
// progress, then the completion, pause or cancel. They used to go out with no
// returnValue, and a caller that goes by returnValue, as luna-send and the
// update service do, took the first progress message as a failure and gave up
// on a download that went on to finish.

#include <gtest/gtest.h>

#include <limits.h>
#include <stdint.h>
#include <string>

#include "DownloadUtils.h"
#include "JUtil.h"

namespace {

pbnjson::JValue parse(const std::string& s)
{
    return JUtil::parse(s.c_str(), std::string(""));
}

// The progress messages as cbWriteEvent and cbReadEvent pasted them together
// before they were built with pbnjson; kept here to show nothing a caller reads
// has changed.
std::string legacyDownloadProgress(unsigned long ticket, uint64_t received, uint64_t total)
{
    return std::string("{ \"ticket\":") + ConvertToString<unsigned long>(ticket)
        + " , \"amountReceived\":" + ConvertToString<uint32_t>((uint32_t)received)
        + " , \"e_amountReceived\":\"" + ConvertToString<uint64_t>(received) + "\""
        + " , \"amountTotal\":" + ConvertToString<uint32_t>((uint32_t)total)
        + " , \"e_amountTotal\":\"" + ConvertToString<uint64_t>(total) + "\""
        + " }";
}

std::string legacyUploadProgress(unsigned long ticket, uint64_t sent, uint64_t total)
{
    return std::string("{ \"ticket\":") + ConvertToString<long>((long)ticket)
        + " , \"amountSent\":" + ConvertToString<int32_t>((int32_t)sent)
        + " , \"e_amountSent\":\"" + ConvertToString<uint64_t>(sent) + "\""
        + " , \"amountTotal\":" + ConvertToString<int32_t>((int32_t)total)
        + " , \"e_amountTotal\":\"" + ConvertToString<uint64_t>(total) + "\""
        + " }";
}

}  // namespace

TEST(SubscriptionUpdate, AddsReturnValueTrue)
{
    pbnjson::JValue j = subscriptionUpdate(downloadProgressJSON(8, 15, 30));
    ASSERT_TRUE(j.hasKey("returnValue"));
    EXPECT_TRUE(j["returnValue"].asBool());
}

TEST(SubscriptionUpdate, KeepsAReturnValueThatIsThere)
{
    pbnjson::JValue failed = pbnjson::Object();
    failed.put("returnValue", false);
    failed.put("errorText", "no space");
    pbnjson::JValue j = subscriptionUpdate(failed);
    EXPECT_FALSE(j["returnValue"].asBool());
    EXPECT_EQ(j["errorText"].asString(), "no space");
}

// The completion and pause messages are the same objects the history db stores;
// its rows have never had returnValue, and resumeDownload() reads them back.
TEST(SubscriptionUpdate, LeavesTheObjectItWasGivenAlone)
{
    pbnjson::JValue history = pbnjson::Object();
    history.put("ticket", 8);
    history.put("completed", true);
    pbnjson::JValue sent = subscriptionUpdate(history);
    EXPECT_TRUE(sent.hasKey("returnValue"));
    EXPECT_FALSE(history.hasKey("returnValue"));
    EXPECT_EQ(history["ticket"].asNumber<int64_t>(), 8);
    EXPECT_TRUE(history["completed"].asBool());
    EXPECT_EQ(JUtil::toSimpleString(sent).size(),
              JUtil::toSimpleString(history).size() + std::string(",\"returnValue\":true").size());
}

TEST(SubscriptionUpdate, SerializesToJsonWithReturnValue)
{
    pbnjson::JValue back = parse(JUtil::toSimpleString(subscriptionUpdate(uploadProgressJSON(3, 1, 2))));
    ASSERT_TRUE(back.isObject());
    EXPECT_TRUE(back["returnValue"].asBool());
}

TEST(SubscriptionUpdate, PassesSomethingThatIsNoObjectThrough)
{
    EXPECT_TRUE(subscriptionUpdate(pbnjson::Array()).isArray());
    EXPECT_TRUE(subscriptionUpdate(pbnjson::JValue()).isNull());
}

TEST(ProgressJson, DownloadCarriesWhatTheHandBuiltOneDid)
{
    const uint64_t cases[][2] = {
        {0, 0}, {15, 30}, {4294967295ull, 4294967295ull},
        {6000000000ull, 9000000000ull},     // past 2^32: amountReceived wraps, e_ does not
    };
    for (const auto& c : cases) {
        pbnjson::JValue legacy = parse(legacyDownloadProgress(424242, c[0], c[1]));
        ASSERT_TRUE(legacy.isObject());
        pbnjson::JValue now = downloadProgressJSON(424242, c[0], c[1]);
        EXPECT_EQ(JUtil::toSimpleString(now), JUtil::toSimpleString(legacy)) << c[0] << "/" << c[1];
    }
}

TEST(ProgressJson, UploadCarriesWhatTheHandBuiltOneDid)
{
    const uint64_t cases[][2] = {
        {0, 0}, {1, 2}, {2147483647ull, 2147483648ull},     // amountTotal went out negative
        {3000000000ull, 6000000000ull},
    };
    for (const auto& c : cases) {
        pbnjson::JValue legacy = parse(legacyUploadProgress(77, c[0], c[1]));
        ASSERT_TRUE(legacy.isObject());
        pbnjson::JValue now = uploadProgressJSON(77, c[0], c[1]);
        EXPECT_EQ(JUtil::toSimpleString(now), JUtil::toSimpleString(legacy)) << c[0] << "/" << c[1];
    }
}

TEST(ProgressJson, TheFullCountsAreInTheStrings)
{
    pbnjson::JValue j = downloadProgressJSON(1, 6000000000ull, 9000000000ull);
    EXPECT_EQ(j["e_amountReceived"].asString(), "6000000000");
    EXPECT_EQ(j["e_amountTotal"].asString(), "9000000000");
    pbnjson::JValue u = uploadProgressJSON(1, 6000000000ull, 9000000000ull);
    EXPECT_EQ(u["e_amountSent"].asString(), "6000000000");
    EXPECT_EQ(u["e_amountTotal"].asString(), "9000000000");
}

// Subscribers are added under subscriptionKey() and answered under it, so the key of a ticket
// is its decimal number and nothing else, also above LONG_MAX where a signed form would differ.
TEST(SubscriptionKey, IsTheTicketInDecimal)
{
    EXPECT_EQ(subscriptionKey(0), "0");
    EXPECT_EQ(subscriptionKey(42), "42");
    EXPECT_EQ(subscriptionKey(4294967295ul), "4294967295");
    EXPECT_EQ(subscriptionKey(ULONG_MAX), std::to_string(ULONG_MAX));
    EXPECT_EQ(subscriptionKey(42), ConvertToString<unsigned long>(42ul));
}
