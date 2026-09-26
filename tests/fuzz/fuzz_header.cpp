// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// The response-header parsing shape from DownloadManager::cbHeader(). The real
// callback needs a live CURL handle registered in m_handleMap, which a fuzz
// target cannot reasonably build, so this mirrors the parsing exactly and drives
// the pieces that are reachable: the trimWhitespace()/tolower() handling and
// DownloadTask's Content-Length and Content-Type consumers.
//
// The bug this guards against is the buffer being treated as NUL-terminated.
// libcurl passes a length and no terminator, so the input here is deliberately
// handed over as (pointer, length) with no terminator of its own.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <algorithm>
#include <string>

#include "DownloadTask.h"
#include "DownloadUtils.h"

namespace {

void parseHeader(const char* headerText, size_t headerSize, DownloadTask& task)
{
    if (headerText == NULL)
        return;

    std::string header(headerText, headerSize);

    size_t labelendpos = header.find(':', 0);
    if (labelendpos == std::string::npos)
        return;

    std::string headerLabel = header.substr(0, labelendpos);
    std::transform(headerLabel.begin(), headerLabel.end(), headerLabel.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });

    std::string headerContent = header.substr(labelendpos + 1, header.size());
    headerContent = trimWhitespace(headerContent);

    if (headerLabel.compare("location") == 0) {
        task.setLocationHeader(headerContent);
        return;
    }

    if (headerLabel.compare("content-length") == 0) {
        uint64_t contentLength = strtouq(headerContent.c_str(), NULL, 10);
        if (contentLength == 0)
            return;
        if ((task.bytesCompleted > 0) && (task.bytesTotal == 0)) {
            task.bytesTotal = contentLength + task.bytesCompleted;
            task.setUpdateInterval();
        }
        else if (task.bytesCompleted == 0) {
            task.bytesTotal = contentLength;
            task.setUpdateInterval();
        }
        // setUpdateInterval() must never produce a zero interval: cbWriteEvent
        // compares against it on every write callback.
        if (task.updateInterval == 0)
            __builtin_trap();
    }
    else if (headerLabel.compare("content-type") == 0) {
        task.setMimeType(headerContent);
        // setMimeType() strips trailing CR/LF; nothing may be left behind, or it
        // ends up inside a JSON reply as a raw control character.
        const std::string& m = task.detectedMIMEType;
        if (!m.empty() && (m[m.size() - 1] == '\r' || m[m.size() - 1] == '\n'))
            __builtin_trap();
    }
}

}   // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // First byte picks the starting state, so the resume fix-up branch
    // ("Content-Length is the remainder") is reachable too.
    DownloadTask task;
    if (size > 0) {
        task.bytesCompleted = (data[0] & 1) ? 4096 : 0;
        task.bytesTotal = (data[0] & 2) ? 0 : 1024 * 1024;
        ++data;
        --size;
    }

    // Split on CRLF the way libcurl hands over one header line per call.
    size_t start = 0;
    for (size_t i = 0; i + 1 < size; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            parseHeader(reinterpret_cast<const char*>(data + start), i + 2 - start, task);
            start = i + 2;
            ++i;
        }
    }
    if (start < size)
        parseHeader(reinterpret_cast<const char*>(data + start), size - start, task);

    return 0;
}
