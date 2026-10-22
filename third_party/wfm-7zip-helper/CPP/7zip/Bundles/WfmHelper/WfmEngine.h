#ifndef WFM_ENGINE_H
#define WFM_ENGINE_H

#include <stdint.h>

struct WfmEngineCallbacks
{
  void *context;
  int (*isCanceled)(void *context);
  void (*setProgress)(void *context, uint64_t done, uint64_t total);
  void (*setCurrentFile)(void *context, const char *path);
};

enum WfmEngineResult
{
  WFM_ENGINE_OK = 0,
  WFM_ENGINE_CANCELED,
  WFM_ENGINE_PASSWORD_REQUIRED,
  WFM_ENGINE_BAD_PASSWORD,
  WFM_ENGINE_UNSUPPORTED_FORMAT,
  WFM_ENGINE_MISSING_VOLUME,
  WFM_ENGINE_UNSAFE_PATH,
  WFM_ENGINE_OVERWRITE_REQUIRED,
  WFM_ENGINE_NO_SPACE,
  WFM_ENGINE_IO_ERROR,
  WFM_ENGINE_DATA_ERROR
};

int WfmEngineExtract(const char *source, const char *destination,
    const char *password, bool overwrite,
    const struct WfmEngineCallbacks *callbacks,
    char *message, unsigned int messageSize);

#endif
