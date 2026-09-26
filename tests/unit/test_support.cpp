// Copyright (c) 2026 LG Electronics, Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// Symbols the daemon defines in Main.cpp. The unit tests link
// LunaDownloadMgrCore, which deliberately excludes Main.cpp so there is exactly
// one main() in the binary (gtest_main's), so the globals it defines have to be
// supplied here.
//
// gMainLoop is only stored (DownloadManager::m_mainLoop) and passed to
// LSGmainAttach()/g_main_loop_quit(), neither of which the tests reach, so a null
// loop is the honest value: anything that starts depending on a real main loop
// will fail visibly rather than run against a half-initialized one.

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

GMainLoop* gMainLoop = NULL;

#ifdef __cplusplus
}
#endif
