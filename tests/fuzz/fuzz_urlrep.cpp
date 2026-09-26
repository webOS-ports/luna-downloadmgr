// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// UrlRep::fromUrl() is the first code to touch the caller-supplied "target" of a
// download request. It wraps uriparser, and getting the failure paths wrong here
// has already produced a leak on every malformed URL and a walk over an
// uninitialized UriQueryListA.

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "UrlRep.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // fromUrl() takes a C string, so the input has to be NUL-terminated here;
    // embedded NULs simply truncate, which is what the real caller does too.
    std::string uri(reinterpret_cast<const char*>(data), size);

    UrlRep rep = UrlRep::fromUrl(uri);

    // Touch everything the callers read, so an invalid parse that leaves a
    // member in a bad state is caught rather than optimised away.
    if (rep.valid) {
        volatile size_t sink = rep.scheme.size() + rep.userInfo.size() +
                               rep.host.size() + rep.port.size() +
                               rep.path.size() + rep.pathOnly.size() +
                               rep.resource.size() + rep.fragment.size();
        (void) sink;

        for (std::map<std::string, std::string>::const_iterator it = rep.query.begin();
             it != rep.query.end(); ++it) {
            volatile size_t s2 = it->first.size() + it->second.size();
            (void) s2;
        }

        // The invariant DownloadManager::download() relies on: a valid parse
        // whose resource is non-empty is used verbatim as the target filename,
        // so it must never contain a path separator.
        if (!rep.resource.empty() && rep.resource.find('/') != std::string::npos)
            __builtin_trap();
    }

    // Exercise the const char* overload's NULL guard on the empty input.
    if (size == 0)
        (void) UrlRep::fromUrl(static_cast<const char*>(NULL));

    return 0;
}
