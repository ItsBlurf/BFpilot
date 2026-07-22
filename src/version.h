/*
 * BFpilot - build version tags.
 */

#pragma once

#ifndef VERSION_TAG
#define VERSION_TAG "bfpilot-v0.4.2-test"
#endif

#ifndef BUILD_VERSION
#define BUILD_VERSION "v0.4.2-test"
#endif

#ifndef BFPILOT_SDK_PATH
#define BFPILOT_SDK_PATH "unknown"
#endif

#ifndef BFPILOT_ENABLE_LAUNCHER
#define BFPILOT_ENABLE_LAUNCHER 0
#endif

#ifndef BFPILOT_DISABLE_LAUNCHER
#define BFPILOT_DISABLE_LAUNCHER 1
#endif

#if BFPILOT_DISABLE_LAUNCHER
#undef BFPILOT_ENABLE_LAUNCHER
#define BFPILOT_ENABLE_LAUNCHER 0
#endif

#ifndef BFPILOT_BUILD_MODE
#define BFPILOT_BUILD_MODE "file-manager"
#endif
