// Copyright (c) 2012-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef DOWNLOADUTILS_H
#define DOWNLOADUTILS_H

#include <stdio.h>
#include <stdint.h>
#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include <luna-service2/lunaservice.h>
#include <pbnjson.hpp>

template <class T> std::string ConvertToString(const T &arg) {
    std::ostringstream      out;
    out << arg;
    return(out.str());
}

// Opens a file of a transfer: "wb" creates or truncates, "ab" creates or appends, "r+b" and "rb" open
// one that is there. Never through a symbolic link (O_NOFOLLOW): the daemon runs as root and writes
// into directories applications share, so a link left under the file's name would have it write
// wherever the link points. The descriptor is close-on-exec. NULL, with errno set, on failure.
FILE* openTransferFile(const std::string& path, const char* mode);
std::string trimWhitespace(const std::string& s,const std::string& drop = "\r\n\t ");
int splitFileAndPath(const std::string& srcPathAndFile,std::string& pathPart,std::string& filePart);
int splitFileAndExtension(const std::string& srcFileAndExt,std::string& filePart,std::string& extensionPart);
int splitStringOnKey(std::vector<std::string>& returnSplitSubstrings,const std::string& baseStr,const std::string& delims);
bool isNonErrorProcExit(int ecode,int normalCode=0);

std::string getHtml5DatabaseFolderNameForApp(const std::string& appId,std::string appFolderPath);

// What a download or upload subscriber is sent while the transfer runs. amountReceived/amountSent and
// amountTotal keep their old 32-bit values, for the callers that read those; e_amountReceived,
// e_amountSent and e_amountTotal carry the full 64-bit counts, as strings.
// The subscription key of a transfer: its ticket in decimal. Every place that adds or answers a
// ticket's subscribers uses this, so they always agree on the key.
std::string subscriptionKey(unsigned long ticket);

pbnjson::JValue downloadProgressJSON(unsigned long ticket, uint64_t received, uint64_t total);
pbnjson::JValue uploadProgressJSON(unsigned long ticket, uint64_t sent, uint64_t total);
// A copy of a subscription message with "returnValue": true added, unless it has one: every reply on
// the bus carries returnValue, and a caller that goes by it took a message without one as a failure.
pbnjson::JValue subscriptionUpdate(const pbnjson::JValue& payload);

#define ERRMASK_POSTSUBUPDATE    1
int postSubscriptionUpdate(const std::string& key,const std::string& postMessage,LSHandle * serviceHandle);
bool processSubscription(LSHandle * serviceHandle, LSMessage * message,const std::string& key);
uint32_t removeSubscriptions(const std::string& key,LSHandle * serviceHandle);

bool doesExistOnFilesystem(const char * pathAndFile);
int64_t filesizeOnFilesystem(const char * pathAndFile);

#endif
