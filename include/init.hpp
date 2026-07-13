#pragma once

#include "defs.hpp"

// Warn if return values are ignored

int mountFilesystems() WARN_UNUSED;
int loadBaseModules() WARN_UNUSED;
int loadAllModules() WARN_UNUSED;
int setupLogging(void *context) WARN_UNUSED;
void flushLogs();
int mountRootfs(void *context) WARN_UNUSED;
int setupAudioGadget(void *context) WARN_UNUSED;
int reloadAudioGadget(void *context) WARN_UNUSED;
int setupNetworkGadget(void *context) WARN_UNUSED;
int bindUsbGadget(void *context) WARN_UNUSED;
