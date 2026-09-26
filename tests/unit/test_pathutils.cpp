// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// The path/string helpers in DownloadUtils decide where a download lands, and
// splitFileAndPath() is called with a mkstemp() result in download(). Several
// of them index with at() and would throw on an empty string, so the empty and
// separator-only inputs are pinned here too.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "DownloadUtils.h"

namespace {

TEST(SplitStringOnKey, SplitsAndDropsEmptyFields)
{
    std::vector<std::string> parts;
    EXPECT_EQ(3, splitStringOnKey(parts, "/media/internal/downloads", "/"));
    ASSERT_EQ(3u, parts.size());
    EXPECT_EQ("media", parts[0]);
    EXPECT_EQ("internal", parts[1]);
    EXPECT_EQ("downloads", parts[2]);
}

TEST(SplitStringOnKey, CollapsesRunsOfDelimiters)
{
    std::vector<std::string> parts;
    EXPECT_EQ(2, splitStringOnKey(parts, "//a////b//", "/"));
    ASSERT_EQ(2u, parts.size());
    EXPECT_EQ("a", parts[0]);
    EXPECT_EQ("b", parts[1]);
}

TEST(SplitStringOnKey, EmptyAndDelimiterOnlyInputsYieldNothing)
{
    std::vector<std::string> parts;
    EXPECT_EQ(0, splitStringOnKey(parts, "", "/"));
    EXPECT_EQ(0, splitStringOnKey(parts, "////", "/"));
    EXPECT_TRUE(parts.empty());
}

TEST(SplitFileAndPath, SplitsAnAbsolutePath)
{
    std::string dir, file;
    EXPECT_GE(splitFileAndPath("/media/internal/downloads/a.bin", dir, file), 2);
    EXPECT_EQ("/media/internal/downloads/", dir);
    EXPECT_EQ("a.bin", file);
}

TEST(SplitFileAndPath, TrailingSlashMeansPathOnly)
{
    std::string dir, file;
    EXPECT_EQ(1, splitFileAndPath("/media/", dir, file));
    EXPECT_EQ("/media/", dir);
    EXPECT_TRUE(file.empty());
}

TEST(SplitFileAndPath, BareFilenameHasNoDirectory)
{
    std::string dir, file;
    EXPECT_EQ(1, splitFileAndPath("a.bin", dir, file));
    EXPECT_TRUE(dir.empty());
    EXPECT_EQ("a.bin", file);
}

TEST(SplitFileAndPath, MkstempShapedInputRoundTrips)
{
    // exactly what download() feeds it for the create-a-temp-file case
    std::string dir, file;
    splitFileAndPath("/media/internal/downloads/fileAb3XyZ", dir, file);
    EXPECT_EQ("/media/internal/downloads/", dir);
    EXPECT_EQ("fileAb3XyZ", file);
}

TEST(SplitFileAndExtension, SplitsOnTheLastDot)
{
    std::string base, ext;
    splitFileAndExtension("archive.tar.gz", base, ext);
    EXPECT_EQ("archive.tar", base);
    EXPECT_EQ("gz", ext);
}

TEST(SplitFileAndExtension, NoDotMeansNoExtension)
{
    std::string base, ext;
    splitFileAndExtension("README", base, ext);
    EXPECT_EQ("README", base);
    EXPECT_TRUE(ext.empty());
}

TEST(TrimWhitespace, TrimsBothEndsAndSurvivesAllWhitespace)
{
    EXPECT_EQ("text/plain", trimWhitespace(" \t text/plain \r\n "));
    EXPECT_EQ("", trimWhitespace("   "));
    EXPECT_EQ("", trimWhitespace(""));
}

TEST(FileSizeOnFilesystem, ReportsSizesWiderThanInt)
{
    // The return type used to be int, which truncated anything over 2GB in
    // getAllHistory's fileSizeOnFilesys. Use a sparse file so the test does not
    // need 3GB of disk.
    const char* path = "ldm_sparse_test.bin";
    const int64_t size = 3LL * 1024 * 1024 * 1024 + 1234;

    FILE* fp = fopen(path, "wb");
    ASSERT_NE(static_cast<FILE*>(NULL), fp);
    ASSERT_EQ(0, fseeko(fp, static_cast<off_t>(size) - 1, SEEK_SET));
    ASSERT_EQ(1u, fwrite("", 1, 1, fp));
    ASSERT_EQ(0, fclose(fp));

    EXPECT_TRUE(doesExistOnFilesystem(path));
    EXPECT_EQ(size, filesizeOnFilesystem(path));

    remove(path);
}

TEST(FileSizeOnFilesystem, MissingAndNullPathsAreZero)
{
    EXPECT_EQ(0, filesizeOnFilesystem("/nonexistent/ldm/path"));
    EXPECT_EQ(0, filesizeOnFilesystem(NULL));
    EXPECT_FALSE(doesExistOnFilesystem(NULL));
}

}   // namespace
