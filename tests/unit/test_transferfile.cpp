// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// openTransferFile(): how the daemon opens the file of a download. It runs as
// root and writes into directories applications share (/media/internal, the
// update service's /userdata/luneos-data), so a symbolic link left under the
// file's name used to make fopen() write wherever the link pointed.

#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#include "DownloadUtils.h"

namespace {

// A directory of the test's own in the build tree, as the other tests use the
// working directory.
struct Dir {
    std::string path;
    Dir() : path("ldm_transferfile_test") { cleanup(); EXPECT_EQ(0, mkdir(path.c_str(), 0700)); }
    ~Dir() { cleanup(); }
    std::string operator/(const char* name) const { return path + "/" + name; }
    void cleanup() const
    {
        for (const char* n : {"victim", "link", "file", "new"})
            (void) unlink((path + "/" + n).c_str());
        (void) rmdir(path.c_str());
    }
};

std::string contents(const std::string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f)
        return "<none>";
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n);
}

void write(const std::string& p, const char* text)
{
    FILE* f = fopen(p.c_str(), "wb");
    ASSERT_NE(static_cast<FILE*>(NULL), f);
    fputs(text, f);
    fclose(f);
}

}  // namespace

TEST(OpenTransferFile, DoesNotWriteThroughASymlink)
{
    Dir d;
    write(d / "victim", "keep me");
    ASSERT_EQ(0, symlink("victim", (d / "link").c_str()));
    for (const char* mode : {"wb", "ab", "r+b", "rb"}) {
        errno = 0;
        EXPECT_EQ(static_cast<FILE*>(NULL), openTransferFile(d / "link", mode)) << mode;
        EXPECT_EQ(ELOOP, errno) << mode;
    }
    EXPECT_EQ("keep me", contents(d / "victim"));
}

TEST(OpenTransferFile, WbCreatesAndTruncates)
{
    Dir d;
    FILE* f = openTransferFile(d / "new", "wb");
    ASSERT_NE(static_cast<FILE*>(NULL), f);
    fputs("abc", f);
    fclose(f);
    EXPECT_EQ("abc", contents(d / "new"));
    write(d / "file", "longer text");
    f = openTransferFile(d / "file", "wb");
    ASSERT_NE(static_cast<FILE*>(NULL), f);
    fputs("x", f);
    fclose(f);
    EXPECT_EQ("x", contents(d / "file"));
}

TEST(OpenTransferFile, AbAppendsAndRPlusBSeeks)
{
    Dir d;
    write(d / "file", "12345");
    FILE* f = openTransferFile(d / "file", "ab");
    ASSERT_NE(static_cast<FILE*>(NULL), f);
    fputs("67", f);
    fclose(f);
    EXPECT_EQ("1234567", contents(d / "file"));

    f = openTransferFile(d / "file", "r+b");
    ASSERT_NE(static_cast<FILE*>(NULL), f);
    ASSERT_EQ(0, fseeko(f, 2, SEEK_SET));
    fputs("ab", f);
    fclose(f);
    EXPECT_EQ("12ab567", contents(d / "file"));
}

TEST(OpenTransferFile, RPlusBAndRbNeedTheFile)
{
    Dir d;
    EXPECT_EQ(static_cast<FILE*>(NULL), openTransferFile(d / "new", "r+b"));
    EXPECT_EQ(ENOENT, errno);
    EXPECT_EQ(static_cast<FILE*>(NULL), openTransferFile(d / "new", "rb"));
    EXPECT_EQ("<none>", contents(d / "new"));
}

TEST(OpenTransferFile, IsCloseOnExec)
{
    Dir d;
    FILE* f = openTransferFile(d / "new", "wb");
    ASSERT_NE(static_cast<FILE*>(NULL), f);
    int fdflags = fcntl(fileno(f), F_GETFD);
    fclose(f);
    EXPECT_NE(0, fdflags & FD_CLOEXEC);
}

TEST(OpenTransferFile, RefusesAModeItDoesNotKnow)
{
    Dir d;
    errno = 0;
    EXPECT_EQ(static_cast<FILE*>(NULL), openTransferFile(d / "new", "w+"));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_EQ(static_cast<FILE*>(NULL), openTransferFile(d / "new", NULL));
    EXPECT_EQ("<none>", contents(d / "new"));
}
