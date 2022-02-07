// Copyright (c) 2013-2018 LG Electronics, Inc.
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

#include "Utils.h"
#include <glib.h>
#include <unistd.h>
#include <ftw.h>
#include <memory.h>
#include <errno.h>
#include <sys/stat.h>

std::string Utils::read_file(const std::string &path)
{
    std::ifstream file(path.c_str());
    if (file.good())
    {
        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();
        return buffer.str();
    }

    return "";
}

bool Utils::make_dir(const std::string &path, bool withParent)
{
    if (!withParent)
    {
        int result = mkdir(path.c_str(), 0755);
        if (result == 0 || errno == EEXIST)
            return true;
    }
    else
    {
        int result = g_mkdir_with_parents(path.c_str(), 0755);
        if (result == 0)
            return true;
    }

    return false;
}

/* Disable unused function
static int rmdir_helper(const char *path, const struct stat *pStat, int flag, struct FTW *ftw)
{
    switch(flag)
    {
    case FTW_D:
    case FTW_DP:
        if (::rmdir(path) == -1)
            return -1;
        break;

    case FTW_F:
    case FTW_SL:
        if (::unlink(path) == -1)
            return -1;
        break;
    }

    return 0;
}
*/

void Utils::remove_file(const std::string &path)
{
    (void) ::unlink(path.c_str());
    return;
}

gboolean Utils::cbAsync(gpointer data)
{
    IAsyncCall *p = reinterpret_cast<IAsyncCall*>(data);
    if (!p) return false;

    p->Call();

    delete p;

    return false;
}
