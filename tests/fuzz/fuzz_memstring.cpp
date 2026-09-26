// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// MemStringToBytes() parses size values out of downloadManager.conf. It copies a
// digit run into a fixed char[32] and used to bound neither the copy nor the
// terminator store, so a long enough run overwrote the stack. Run this with
// -fsanitize=address to keep that fixed.

#include <stddef.h>
#include <stdint.h>
#include <string>

// Declared in DownloadSettings.cpp; deliberately not exported through a header.
unsigned long MemStringToBytes(const char* ptr);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string value(reinterpret_cast<const char*>(data), size);
    volatile unsigned long r = MemStringToBytes(value.c_str());
    (void) r;

    if (size == 0)
        (void) MemStringToBytes(NULL);

    return 0;
}
