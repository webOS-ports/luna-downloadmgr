// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// filesystemStatusCheck() converts free/total KB into the percentage the
// filesysStatusCheck method reports and into the stop-mark decision that
// download() and resumeDownload() use to refuse new transfers. It is reachable
// with a caller-chosen path, so the degenerate filesystem shapes matter: a
// zero-sized filesystem made it divide 0.0 by 0.0 and then convert NaN to
// uint32_t, which is undefined.

#include <gtest/gtest.h>

#include <stdint.h>

#include "DownloadManager.h"
#include "DownloadSettings.h"

namespace {

uint32_t pctFullFor(uint64_t freeKB, uint64_t totalKB)
{
    uint32_t pct = 0;
    DownloadManager::instance().filesystemStatusCheck(freeKB, totalKB, &pct, NULL);
    return pct;
}

TEST(FilesystemStatusCheck, ReportsThePercentageInUse)
{
    EXPECT_EQ(0u,   pctFullFor(1000, 1000));
    EXPECT_EQ(50u,  pctFullFor(500, 1000));
    EXPECT_EQ(90u,  pctFullFor(100, 1000));
    EXPECT_EQ(100u, pctFullFor(0, 1000));
}

TEST(FilesystemStatusCheck, AZeroSizedFilesystemIsFull)
{
    // statvfs() reports f_blocks == 0 for procfs, sysfs and cgroupfs, and
    // filesysStatusCheck stats whatever path the caller passes in.
    EXPECT_EQ(100u, pctFullFor(0, 0));
    EXPECT_EQ(100u, pctFullFor(1024, 0));
}

TEST(FilesystemStatusCheck, NeverReportsMoreThanAHundred)
{
    // free > total is nonsense but observable while a filesystem is resizing
    EXPECT_LE(pctFullFor(2000, 1000), 100u);
}

TEST(FilesystemStatusCheck, HandlesSizesBeyondThirtyTwoBits)
{
    const uint64_t totalKB = 8ull * 1024 * 1024 * 1024;   // 8 TB in KB
    EXPECT_EQ(50u, pctFullFor(totalKB / 2, totalKB));
    EXPECT_EQ(0u,  pctFullFor(totalKB, totalKB));
}

TEST(FilesystemStatusCheck, StopMarkTracksTheConfiguredRemainder)
{
    const uint64_t stopKB = DownloadSettings::instance().freespaceStopmarkRemainingKBytes;
    const uint64_t totalKB = 1024ull * 1024;

    bool stop = false;
    // Comfortably above the mark and below the low-water percentage: no stop.
    DownloadManager::instance().filesystemStatusCheck(totalKB, totalKB, NULL, &stop);
    EXPECT_FALSE(stop);

    // At or below the configured remainder, and full enough to be evaluated.
    stop = false;
    DownloadManager::instance().filesystemStatusCheck(stopKB, totalKB, NULL, &stop);
    EXPECT_TRUE(stop);
}

TEST(FilesystemStatusCheck, NullOutParametersAreAllowed)
{
    EXPECT_NO_THROW(
        DownloadManager::instance().filesystemStatusCheck(1, 1000, NULL, NULL));
}

TEST(ConnectionNames, RoundTripThroughIdAndBack)
{
    // The interface name is persisted in the history db and read back by
    // resumeDownload(); an id/name pair that does not round-trip silently
    // resumes on the wrong interface.
    const DownloadManager::Connection ids[] = {
        DownloadManager::Wired, DownloadManager::Wifi,
        DownloadManager::Wan,   DownloadManager::Btpan,
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        std::string name = DownloadManager::connectionId2Name(ids[i]);
        EXPECT_EQ(ids[i], DownloadManager::connectionName2Id(name)) << "name=" << name;
    }

    EXPECT_EQ("*", DownloadManager::connectionId2Name(DownloadManager::ANY));
    EXPECT_EQ(DownloadManager::ANY, DownloadManager::connectionName2Id("*"));
    EXPECT_EQ(DownloadManager::ANY, DownloadManager::connectionName2Id("nonsense"));
    EXPECT_EQ(DownloadManager::ANY, DownloadManager::connectionName2Id(""));
}

}   // namespace
