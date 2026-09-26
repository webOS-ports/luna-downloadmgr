// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// UploadTask builds the multipart body. Moving it from curl_formadd() to the
// mime API changes how every part is described to libcurl, so this checks the
// body libcurl actually produces rather than just that the calls returned
// CURLE_OK: it runs a real transfer against a throwaway loopback HTTP server and
// inspects what arrived.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "UploadTask.h"

namespace {

// ---------------------------------------------------------------- tiny server

// Accepts one connection, reads the whole request, replies 200, and hands the
// request back to the test. Deliberately minimal: it only has to be enough for
// libcurl to complete a POST against.
struct OneShotServer
{
    int         listenFd = -1;
    uint16_t    port = 0;
    pthread_t   thread = 0;
    std::string request;

    bool start()
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        int on = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;                      // let the kernel pick
        if (::bind(listenFd, (struct sockaddr *) &addr, sizeof(addr)) != 0) return false;
        if (::listen(listenFd, 1) != 0) return false;

        socklen_t len = sizeof(addr);
        if (::getsockname(listenFd, (struct sockaddr *) &addr, &len) != 0) return false;
        port = ntohs(addr.sin_port);

        return pthread_create(&thread, NULL, &OneShotServer::run, this) == 0;
    }

    void join()
    {
        if (thread) { pthread_join(thread, NULL); thread = 0; }
        if (listenFd >= 0) { ::close(listenFd); listenFd = -1; }
    }

    std::string url(const char *path = "/upload") const
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "http://127.0.0.1:%u%s", (unsigned) port, path);
        return buf;
    }

private:
    static void *run(void *self)
    {
        OneShotServer *s = static_cast<OneShotServer *>(self);
        int fd = ::accept(s->listenFd, NULL, NULL);
        if (fd < 0) return NULL;

        // Read until the client stops sending. libcurl closes its side after
        // the body because we answer with Connection: close.
        char buf[8192];
        ssize_t n;
        size_t headerEnd = std::string::npos;
        size_t contentLength = 0;
        bool haveLength = false;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
            s->request.append(buf, (size_t) n);
            if (headerEnd == std::string::npos) {
                headerEnd = s->request.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    size_t cl = s->request.find("Content-Length:");
                    if (cl != std::string::npos && cl < headerEnd) {
                        contentLength = (size_t) strtoul(s->request.c_str() + cl + 15, NULL, 10);
                        haveLength = true;
                    }
                }
            }
            if (haveLength && s->request.size() >= headerEnd + 4 + contentLength)
                break;
        }

        static const char reply[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        (void) ::send(fd, reply, sizeof(reply) - 1, 0);
        ::close(fd);
        return NULL;
    }
};

std::string writeTempFile(const std::string &name, const std::string &body)
{
    std::string path = "./" + name;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return std::string();
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return path;
}

bool contains(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

// Drive one upload to completion synchronously. UploadTask normally runs under
// glibcurl's multi handle; for the test, curl_easy_perform() on the same handle
// exercises exactly the options UploadTask set.
bool perform(UploadTask *t)
{
    return curl_easy_perform(t->getCURLHandlePtr()) == CURLE_OK;
}

// ---------------------------------------------------------------------- tests

TEST(UploadMime, FileUploadSendsAMultipartBodyWithTheFileAndItsParts)
{
    OneShotServer srv;
    ASSERT_TRUE(srv.start());

    std::string path = writeTempFile("ldm_upload_payload.bin", "FILE-CONTENTS-1234567890");
    ASSERT_FALSE(path.empty());

    std::vector<PostItem>    parts;
    parts.push_back(PostItem("plainkey", "plainvalue", PostItem::Value, ""));
    parts.push_back(PostItem("typedkey", "{\"j\":1}", PostItem::Value, "application/json"));
    std::vector<std::string> headers;
    headers.push_back("X-Test-Header: present");
    std::vector<kvpair>      cookies;
    cookies.push_back(kvpair("sid", "abc123"));

    UploadTask *t = UploadTask::newFileUploadTask(srv.url(), path, "thefile",
                                                  parts, headers, cookies,
                                                  "application/octet-stream");
    ASSERT_NE(static_cast<UploadTask *>(NULL), t);
    EXPECT_TRUE(perform(t));
    srv.join();
    delete t;
    remove(path.c_str());

    const std::string &req = srv.request;
    ASSERT_FALSE(req.empty());

    // multipart, not urlencoded - CURLOPT_MIMEPOST has to behave like
    // CURLOPT_HTTPPOST did
    EXPECT_TRUE(contains(req, "Content-Type: multipart/form-data")) << req.substr(0, 400);
    EXPECT_TRUE(contains(req, "POST /upload"));

    // every part name survives, under the names curl_mime_name() was given
    EXPECT_TRUE(contains(req, "name=\"plainkey\""));
    EXPECT_TRUE(contains(req, "name=\"typedkey\""));
    EXPECT_TRUE(contains(req, "name=\"thefile\""));

    // inline part data, from curl_mime_data()
    EXPECT_TRUE(contains(req, "plainvalue"));
    EXPECT_TRUE(contains(req, "{\"j\":1}"));

    // the file's contents and its derived filename, from curl_mime_filedata()
    EXPECT_TRUE(contains(req, "FILE-CONTENTS-1234567890"));
    EXPECT_TRUE(contains(req, "filename=\"ldm_upload_payload.bin\""));

    // per-part content types, from curl_mime_type()
    EXPECT_TRUE(contains(req, "Content-Type: application/json"));
    EXPECT_TRUE(contains(req, "Content-Type: application/octet-stream"));

    // custom headers and cookies still applied
    EXPECT_TRUE(contains(req, "X-Test-Header: present"));
    EXPECT_TRUE(contains(req, "Cookie: sid=abc123"));
}

TEST(UploadMime, APartWithNoContentTypeGetsNoEmptyContentTypeHeader)
{
    // curl_formadd() skipped CURLFORM_CONTENTTYPE for an empty string;
    // curl_mime_type(part, "") would emit "Content-Type: ", so the migration
    // has to skip it too.
    OneShotServer srv;
    ASSERT_TRUE(srv.start());

    std::string path = writeTempFile("ldm_upload_notype.bin", "x");
    std::vector<PostItem>    parts;
    parts.push_back(PostItem("bare", "value", PostItem::Value, ""));
    std::vector<std::string> headers;
    std::vector<kvpair>      cookies;

    UploadTask *t = UploadTask::newFileUploadTask(srv.url(), path, "thefile",
                                                  parts, headers, cookies, "");
    ASSERT_NE(static_cast<UploadTask *>(NULL), t);
    EXPECT_TRUE(perform(t));
    srv.join();
    delete t;
    remove(path.c_str());

    EXPECT_TRUE(contains(srv.request, "name=\"bare\""));
    EXPECT_FALSE(contains(srv.request, "Content-Type: \r\n")) << srv.request;
}

TEST(UploadMime, BufferUploadCopiesItsBody)
{
    // CURLOPT_COPYPOSTFIELDS, not CURLOPT_POSTFIELDS: the source string is
    // destroyed before the transfer runs, so a non-copying option would send
    // freed memory.
    OneShotServer srv;
    ASSERT_TRUE(srv.start());

    std::vector<std::string> headers;
    UploadTask *t;
    {
        std::string body = "BUFFER-BODY-abcdefghijklmnop";
        t = UploadTask::newBufferUploadTask(srv.url("/buf"), body, headers, "text/plain");
        ASSERT_NE(static_cast<UploadTask *>(NULL), t);
        // body goes out of scope here, before the transfer
    }
    EXPECT_TRUE(perform(t));
    srv.join();
    delete t;

    EXPECT_TRUE(contains(srv.request, "POST /buf"));
    EXPECT_TRUE(contains(srv.request, "BUFFER-BODY-abcdefghijklmnop")) << srv.request;
}

TEST(UploadMime, TaskIdsAreUniqueAndNonZero)
{
    // 0 means "invalid upload id" to uploadPOSTFile()'s callers.
    OneShotServer srv;
    ASSERT_TRUE(srv.start());

    std::vector<PostItem>    parts;
    std::vector<std::string> headers;
    std::vector<kvpair>      cookies;
    std::string path = writeTempFile("ldm_upload_ids.bin", "y");

    UploadTask *a = UploadTask::newFileUploadTask(srv.url(), path, "f", parts, headers, cookies, "");
    UploadTask *b = UploadTask::newFileUploadTask(srv.url(), path, "f", parts, headers, cookies, "");
    ASSERT_NE(static_cast<UploadTask *>(NULL), a);
    ASSERT_NE(static_cast<UploadTask *>(NULL), b);

    EXPECT_NE(0u, a->id());
    EXPECT_NE(0u, b->id());
    EXPECT_NE(a->id(), b->id());
    EXPECT_EQ(path, a->source());
    EXPECT_EQ(srv.url(), a->url());

    delete a;
    delete b;
    remove(path.c_str());

    // nothing connected; shut the listener down
    ::shutdown(srv.listenFd, SHUT_RDWR);
    srv.join();
}

TEST(UploadMime, DestroyingATaskReleasesTheMimeStructure)
{
    // curl_mime_free() must happen before curl_easy_cleanup(); getting that
    // order wrong is a use-after-free that only ASan would otherwise catch.
    OneShotServer srv;
    ASSERT_TRUE(srv.start());

    std::string path = writeTempFile("ldm_upload_free.bin", "z");
    for (int i = 0; i < 25; ++i) {
        std::vector<PostItem>    parts;
        parts.push_back(PostItem("k", "v", PostItem::Value, "text/plain"));
        std::vector<std::string> headers;
        headers.push_back("X-Iter: y");
        std::vector<kvpair>      cookies;
        cookies.push_back(kvpair("c", "d"));

        UploadTask *t = UploadTask::newFileUploadTask(srv.url(), path, "f",
                                                      parts, headers, cookies, "text/plain");
        ASSERT_NE(static_cast<UploadTask *>(NULL), t);
        delete t;       // never performed: exercises the teardown path alone
    }
    remove(path.c_str());

    ::shutdown(srv.listenFd, SHUT_RDWR);
    srv.join();
    SUCCEED();
}

}   // namespace
