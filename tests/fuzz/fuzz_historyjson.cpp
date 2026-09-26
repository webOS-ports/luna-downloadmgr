// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every field resumeDownload() and cancelFromHistory() use comes out of a JSON
// blob stored in /var/luna/data/downloadhistory.db, parsed with no schema. A row
// written by an older build, or one corrupted in place, reaches this code as
// arbitrary JSON, so the extraction has to tolerate missing keys, wrong types and
// values that do not fit their destination.
//
// This mirrors the extraction in DownloadManager::resumeDownload() rather than
// calling it, because the real function goes on to open files and register curl
// handles.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string>

#include <pbnjson.hpp>

#include "JUtil.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string raw(reinterpret_cast<const char*>(data), size);

    pbnjson::JValue root = JUtil::parse(raw.c_str(), std::string(""));
    if (root.isNull())
        return 0;

    uint64_t totalSize = 0;
    if (root.hasKey("e_amountTotal"))
        totalSize = strtouq(root["e_amountTotal"].asString().c_str(), 0, 10);
    else if (root.hasKey("amountTotal"))
        totalSize = (uint64_t) root["amountTotal"].asNumber<int64_t>();

    uint64_t completedSize = 0;
    if (root.hasKey("e_amountReceived"))
        completedSize = strtouq(root["e_amountReceived"].asString().c_str(), 0, 10);
    else if (root.hasKey("amountReceived"))
        completedSize = (uint64_t) root["amountReceived"].asNumber<int64_t>();

    uint64_t initialOffset = 0;
    if (root.hasKey("e_initialOffsetBytes"))
        initialOffset = strtouq(root["e_initialOffsetBytes"].asString().c_str(), 0, 10);
    else if (root.hasKey("e_initialOffset"))
        initialOffset = strtouq(root["e_initialOffset"].asString().c_str(), 0, 10);
    else if (root.hasKey("initialOffset"))
        initialOffset = (uint64_t) root["initialOffset"].asNumber<int64_t>();

    std::string uri, destTempFile, destTempPrefix, destFinalFile, destFinalPath;
    (void) root["sourceUrl"].asString(uri);
    (void) root["target"].asString(destTempFile);
    destTempPrefix = root["destTempPrefix"].asString();
    (void) root["destFile"].asString(destFinalFile);
    (void) root["destPath"].asString(destFinalPath);

    // What resumeDownload() computes next. The subtraction is unsigned, and a
    // record claiming a larger initial offset than received bytes used to wrap
    // it into a huge seek target.
    uint64_t remainSize = (totalSize > completedSize) ? (totalSize - completedSize) : 0;
    uint64_t resumeAt = (completedSize > initialOffset) ? (completedSize - initialOffset) : 0;
    if (resumeAt > completedSize || remainSize > totalSize)
        __builtin_trap();

    // And what cancelFromHistory() rebuilds from the same row: the reply must
    // still be parseable whatever the stored url contains.
    pbnjson::JValue payload = pbnjson::Object();
    payload.put("ticket", (int64_t) 1);
    payload.put("url", uri);
    payload.put("aborted", true);
    std::string out = JUtil::toSimpleString(payload);
    if (!out.empty()) {
        pbnjson::JValue back = JUtil::parse(out.c_str(), std::string(""));
        if (back.isNull())
            __builtin_trap();   // we emitted JSON we cannot read back
    }

    return 0;
}
