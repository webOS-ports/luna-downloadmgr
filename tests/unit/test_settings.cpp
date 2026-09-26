// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// DownloadSettings reads /etc/palm/downloadManager.conf. Two pieces of that are
// reachable with hostile-shaped input: validateDownloadPath(), which is the only
// thing keeping the download directory inside /var or /media, and
// MemStringToBytes(), which used to copy an unbounded digit run into a char[32].

#include <gtest/gtest.h>

#include <climits>
#include <string>

#include "DownloadSettings.h"

// Declared in DownloadSettings.cpp; not exported through a header.
unsigned long MemStringToBytes(const char* ptr);

namespace {

TEST(ValidateDownloadPath, AcceptsVarAndMedia)
{
    EXPECT_TRUE(DownloadSettings::validateDownloadPath("/media/internal/downloads"));
    EXPECT_TRUE(DownloadSettings::validateDownloadPath("/var/luna/downloads"));
}

TEST(ValidateDownloadPath, RejectsEscapesAndOtherRoots)
{
    EXPECT_FALSE(DownloadSettings::validateDownloadPath("/etc"));
    EXPECT_FALSE(DownloadSettings::validateDownloadPath("/"));
    EXPECT_FALSE(DownloadSettings::validateDownloadPath(""));
    EXPECT_FALSE(DownloadSettings::validateDownloadPath("media/internal"));       // not anchored
    EXPECT_FALSE(DownloadSettings::validateDownloadPath("/media/../etc"));
    EXPECT_FALSE(DownloadSettings::validateDownloadPath("/var/../../etc"));
    EXPECT_FALSE(DownloadSettings::validateDownloadPath("/media/internal/.."));
}

TEST(MemStringToBytes, ParsesPlainAndSuffixedValues)
{
    EXPECT_EQ(0ul, MemStringToBytes("0"));
    EXPECT_EQ(1024ul, MemStringToBytes("1024"));
    EXPECT_EQ(2048ul, MemStringToBytes("2k"));
    EXPECT_EQ(2048ul, MemStringToBytes("2K"));
    EXPECT_EQ(1048576ul, MemStringToBytes("1M"));
    EXPECT_EQ(64ul * 1024, MemStringToBytes("  64k"));
}

TEST(MemStringToBytes, UnknownSuffixesAreIgnored)
{
    EXPECT_EQ(5ul, MemStringToBytes("5G"));
    EXPECT_EQ(5ul, MemStringToBytes("5 bananas"));
}

TEST(MemStringToBytes, LongDigitRunsDoNotOverflowTheBuffer)
{
    // The implementation copies the digit run into a char[32]. A longer run used
    // to run off the end of that buffer; under ASan this test is the regression
    // check, and without it the assertion below at least pins the truncation.
    std::string digits(200, '9');
    EXPECT_NO_THROW((void) MemStringToBytes(digits.c_str()));

    std::string withSuffix = std::string(64, '1') + "M";
    EXPECT_NO_THROW((void) MemStringToBytes(withSuffix.c_str()));
}

TEST(MemStringToBytes, DegenerateInputsAreHandled)
{
    EXPECT_EQ(0ul, MemStringToBytes(""));
    EXPECT_EQ(0ul, MemStringToBytes("   "));
    EXPECT_EQ(0ul, MemStringToBytes("M"));
    EXPECT_EQ(0ul, MemStringToBytes(NULL));
    // High-bit bytes must not be handed to isdigit()/isalnum() as negative chars
    EXPECT_NO_THROW((void) MemStringToBytes("\xff\xfe\x80" "12k"));
}

TEST(DownloadSettings, DefaultsAreSane)
{
    const DownloadSettings& s = DownloadSettings::instance();

    EXPECT_TRUE(DownloadSettings::validateDownloadPath(s.downloadPathMedia));
    EXPECT_GT(s.maxDownloadManagerQueueLength, 0u);
    EXPECT_GT(s.maxDownloadManagerConcurrent, 0);
    // the alert marks have to stay strictly increasing or filesysStatusCheck
    // reports the wrong band
    EXPECT_LT(s.freespaceLowmarkFullPercent, s.freespaceMedmarkFullPercent);
    EXPECT_LT(s.freespaceMedmarkFullPercent, s.freespaceHighmarkFullPercent);
    EXPECT_LT(s.freespaceHighmarkFullPercent, s.freespaceCriticalmarkFullPercent);
    EXPECT_LT(s.freespaceCriticalmarkFullPercent, 100u);
}

}   // namespace
