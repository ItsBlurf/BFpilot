// Main.cpp
// Long-running PS5 daemon entry point for bfpilot-7zip.

#include "WfmProtocol.h"
#include "WfmEngine.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef WFM_PLATFORM_PS5
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#else
#include <dirent.h>
#include <sys/prctl.h>
#endif

#define WFM_HELPER_VERSION "BF7Z/1 26.03-2-bf2"
#define WFM_HELPER_PROCESS_NAME "bfpilot-7zip"

#ifdef WFM_PLATFORM_PS5
#define WFM_HELPER_SOCKET "/system_tmp/bfpilot-7zip.sock"
#define WFM_HELPER_LOCK "/system_tmp/bfpilot-7zip.lock"

struct NotifyRequest
{
  char unused[45];
  char message[3075];
};

extern "C" int sceKernelSendNotificationRequest(int device,
    struct NotifyRequest *request, size_t size, int flags);
#else
#define WFM_HELPER_SOCKET "/tmp/bfpilot-7zip.sock"
#define WFM_HELPER_LOCK "/tmp/bfpilot-7zip.lock"
#endif

static volatile sig_atomic_t gStopping;
static int gServerFd = -1;
static pthread_mutex_t gTaskMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gSendMutex = PTHREAD_MUTEX_INITIALIZER;

struct ExtractWork
{
  int clientFd;
  uint64_t requestId;
  char **sources;
  char **destinations;
  size_t itemCount;
  char *password;
  int overwrite;
  int cancelRequested;
  uint32_t state;
  uint32_t currentIndex;
  uint32_t completedCount;
  uint64_t total;
  uint64_t done;
  uint64_t itemBaseDone;
  uint64_t totalPackSize;
  uint64_t itemPackSize;
  char *current;
  char *errorCode;
  char *errorMessage;
  struct ExtractWork *next;
};

#define WFM_TASK_RUNNING 1U
#define WFM_TASK_DONE 2U
#define WFM_TASK_FAILED 3U
#define WFM_TASK_CANCELED 4U

static struct ExtractWork *gTasks;

/* Caller holds gTaskMutex. */
static struct ExtractWork *FindTaskLocked(uint64_t requestId)
{
  for (struct ExtractWork *work = gTasks; work; work = work->next)
    if (work->requestId == requestId)
      return work;
  return NULL;
}

static void SetProcessName(void)
{
#ifdef WFM_PLATFORM_PS5
  (void)syscall(SYS_thr_set_name, -1, WFM_HELPER_PROCESS_NAME);
#else
  (void)prctl(PR_SET_NAME, WFM_HELPER_PROCESS_NAME, 0, 0, 0);
#endif
}

static void StopHandler(int signalNumber)
{
  (void)signalNumber;
  __atomic_store_n(&gStopping, 1, __ATOMIC_RELAXED);
}

static int AcquireInstanceLock(void)
{
  const int fd = open(WFM_HELPER_LOCK, O_CREAT | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0)
  {
    const int savedErrno = errno;
    close(fd);
    errno = savedErrno;
    return -1;
  }
  return fd;
}

static int CreateServer(void)
{
  struct sockaddr_un address;
  const size_t pathLen = strlen(WFM_HELPER_SOCKET);
  mode_t oldMask;
  int fd;

  if (pathLen >= sizeof(address.sun_path))
  {
    errno = ENAMETOOLONG;
    return -1;
  }

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, WFM_HELPER_SOCKET, pathLen + 1);
#ifdef WFM_PLATFORM_PS5
  address.sun_len = (unsigned char)SUN_LEN(&address);
  const socklen_t addressLen = address.sun_len;
#else
  const socklen_t addressLen = (socklen_t)(offsetof(struct sockaddr_un,
      sun_path) + pathLen + 1);
#endif

  unlink(WFM_HELPER_SOCKET);
  oldMask = umask(0077);
  const int bindResult = bind(fd, (struct sockaddr *)&address, addressLen);
  umask(oldMask);
  if (bindResult != 0)
  {
    const int savedErrno = errno;
    close(fd);
    errno = savedErrno;
    return -1;
  }

  if (listen(fd, 4) != 0)
  {
    const int savedErrno = errno;
    close(fd);
    unlink(WFM_HELPER_SOCKET);
    errno = savedErrno;
    return -1;
  }
  return fd;
}

static int SendEmptyFrame(int fd, uint16_t type, uint64_t requestId)
{
  int result;
  pthread_mutex_lock(&gSendMutex);
  result = WfmSendFrame(fd, type, 0, requestId, NULL, 0);
  pthread_mutex_unlock(&gSendMutex);
  return result;
}

static int SendError(int fd, uint64_t requestId, const char *code,
    const char *message)
{
  unsigned char payload[512];
  const size_t codeLength = strlen(code);
  const size_t messageLength = strlen(message);
  int result;

  if (codeLength + messageLength + 8 > sizeof(payload))
  {
    errno = EMSGSIZE;
    return -1;
  }
  WfmWrite32(payload, (uint32_t)codeLength);
  WfmWrite32(payload + 4, (uint32_t)messageLength);
  memcpy(payload + 8, code, codeLength);
  memcpy(payload + 8 + codeLength, message, messageLength);
  pthread_mutex_lock(&gSendMutex);
  result = WfmSendFrame(fd, WFM_MSG_ERROR, 0, requestId, payload,
      (uint32_t)(codeLength + messageLength + 8));
  pthread_mutex_unlock(&gSendMutex);
  return result;
}

static void ClearExtractWork(struct ExtractWork *work)
{
  if (!work)
    return;
  for (size_t i = 0; i < work->itemCount; ++i)
  {
    free(work->sources ? work->sources[i] : NULL);
    free(work->destinations ? work->destinations[i] : NULL);
  }
  free(work->sources);
  free(work->destinations);
  free(work->current);
  free(work->errorCode);
  free(work->errorMessage);
  if (work->password)
  {
    memset(work->password, 0, strlen(work->password));
    free(work->password);
  }
  free(work);
}

static char *CopyProtocolString(const unsigned char *data, uint32_t length)
{
  char *text;
  if (length && memchr(data, 0, length))
    return NULL;
  text = (char *)malloc((size_t)length + 1);
  if (!text)
    return NULL;
  memcpy(text, data, length);
  text[length] = 0;
  return text;
}

static int ParseExtractRequest(const char *payload, uint32_t payloadSize,
    struct ExtractWork *work)
{
  const unsigned char *data = (const unsigned char *)payload;
  uint32_t passwordLength;
  uint32_t itemCount;
  uint64_t offset;

  if (payloadSize < 16 || WfmRead32(data) != 0 || WfmRead32(data + 12) != 0)
    return -1;
  passwordLength = WfmRead32(data + 4);
  itemCount = WfmRead32(data + 8);
  if (!itemCount || itemCount > 1024 ||
      passwordLength > WFM_HELPER_MAX_PASSWORD ||
      16ULL + passwordLength > payloadSize)
    return -1;

  work->itemCount = itemCount;
  work->password = CopyProtocolString(data + 16, passwordLength);
  work->sources = (char **)calloc(itemCount, sizeof(*work->sources));
  work->destinations = (char **)calloc(itemCount, sizeof(*work->destinations));
  if (!work->password || !work->sources || !work->destinations)
    return -1;

  offset = 16ULL + passwordLength;
  for (uint32_t i = 0; i < itemCount; ++i)
  {
    uint32_t sourceLength;
    uint32_t destinationLength;
    if ((uint64_t)payloadSize - offset < 8)
      return -1;
    sourceLength = WfmRead32(data + offset);
    destinationLength = WfmRead32(data + offset + 4);
    offset += 8;
    if (!sourceLength || !destinationLength ||
        sourceLength > WFM_HELPER_MAX_PATH ||
        destinationLength > WFM_HELPER_MAX_PATH ||
        (uint64_t)sourceLength + destinationLength >
        (uint64_t)payloadSize - offset)
      return -1;
    work->sources[i] = CopyProtocolString(data + offset, sourceLength);
    offset += sourceLength;
    work->destinations[i] = CopyProtocolString(data + offset,
        destinationLength);
    offset += destinationLength;
    if (!work->sources[i] || !work->destinations[i])
      return -1;
  }

  /* Future optional fields are appended as field-id, length and value. */
  while (offset < payloadSize)
  {
    uint32_t extensionId;
    uint32_t extensionLength;
    if ((uint64_t)payloadSize - offset < 8)
      return -1;
    extensionId = WfmRead32(data + offset);
    extensionLength = WfmRead32(data + offset + 4);
    offset += 8;
    if (extensionLength > (uint64_t)payloadSize - offset)
      return -1;
    if (extensionId == 1)
    {
      if (extensionLength != 1 || data[offset] > 1) return -1;
      work->overwrite = data[offset];
    }
    offset += extensionLength;
  }
  return 0;
}

static int EngineIsCanceled(void *context)
{
  struct ExtractWork *work = (struct ExtractWork *)context;
  int canceled;
  pthread_mutex_lock(&gTaskMutex);
  canceled = work->cancelRequested || __atomic_load_n(&gStopping, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&gTaskMutex);
  return canceled;
}

static uint64_t ScaleProgress(uint64_t done, uint64_t total,
    uint64_t archiveSize)
{
  const uint64_t limit = (uint64_t)1 << 31;
  while (archiveSize > limit) { archiveSize >>= 1; total >>= 1; }
  while (done > limit) { done >>= 1; total >>= 1; }
  return done * archiveSize / (total ? total : 1);
}

static void EngineSetProgress(void *context, uint64_t done, uint64_t total)
{
  struct ExtractWork *work = (struct ExtractWork *)context;
  unsigned char payload[16];
  int fd;
  pthread_mutex_lock(&gTaskMutex);
  if (work->itemCount > 1 && work->totalPackSize)
  {
    work->total = work->totalPackSize;
    work->done = work->itemBaseDone +
        (total ? ScaleProgress(done, total, work->itemPackSize) : 0);
  }
  else
  {
    work->total = total;
    work->done = done;
  }
  WfmWrite64(payload, work->total);
  WfmWrite64(payload + 8, work->done);
  fd = work->clientFd;
  if (fd >= 0)
  {
    pthread_mutex_lock(&gSendMutex);
    (void)WfmSendFrame(fd, WFM_MSG_PROGRESS, 0, work->requestId,
        payload, sizeof(payload));
    pthread_mutex_unlock(&gSendMutex);
  }
  pthread_mutex_unlock(&gTaskMutex);
}

static void EngineSetCurrentFile(void *context, const char *path)
{
  struct ExtractWork *work = (struct ExtractWork *)context;
  const size_t length = strlen(path);
  unsigned char *payload = NULL;
  int fd;
  if (length <= WFM_HELPER_MAX_PATH &&
      (payload = (unsigned char *)malloc(length + 4)) != NULL)
  {
    WfmWrite32(payload, (uint32_t)length);
    memcpy(payload + 4, path, length);
  }
  pthread_mutex_lock(&gTaskMutex);
  free(work->current);
  work->current = strdup(path);
  fd = work->clientFd;
  if (fd >= 0 && payload)
  {
    pthread_mutex_lock(&gSendMutex);
    (void)WfmSendFrame(fd, WFM_MSG_CURRENT_FILE, 0, work->requestId,
        payload, (uint32_t)(length + 4));
    pthread_mutex_unlock(&gSendMutex);
  }
  pthread_mutex_unlock(&gTaskMutex);
  free(payload);
}

static const char *EngineErrorCode(int result)
{
  switch (result)
  {
    case WFM_ENGINE_CANCELED: return "canceled";
    case WFM_ENGINE_PASSWORD_REQUIRED: return "archive_password_required";
    case WFM_ENGINE_BAD_PASSWORD: return "archive_bad_password";
    case WFM_ENGINE_UNSUPPORTED_FORMAT: return "archive_unsupported_format";
    case WFM_ENGINE_MISSING_VOLUME: return "archive_missing_volume";
    case WFM_ENGINE_UNSAFE_PATH: return "archive_unsafe_path";
    case WFM_ENGINE_OVERWRITE_REQUIRED: return "archive_overwrite_required";
    case WFM_ENGINE_NO_SPACE: return "archive_no_space";
    case WFM_ENGINE_IO_ERROR: return "archive_io_error";
    default: return "archive_extract_failed";
  }
}

static void *ExtractWorker(void *argument)
{
  struct ExtractWork *work = (struct ExtractWork *)argument;
  struct WfmEngineCallbacks callbacks;
  int engineResult = WFM_ENGINE_OK;
  char message[256] = { 0 };

  callbacks.context = work;
  callbacks.isCanceled = EngineIsCanceled;
  callbacks.setProgress = EngineSetProgress;
  callbacks.setCurrentFile = EngineSetCurrentFile;

  if (work->itemCount > 1)
  {
    for (size_t i = 0; i < work->itemCount; ++i)
    {
      struct stat st;
      if (stat(work->sources[i], &st) == 0 && st.st_size > 0 &&
          (uint64_t)st.st_size <= UINT64_MAX - work->totalPackSize)
      {
        work->totalPackSize += (uint64_t)st.st_size;
      }
    }
  }

  for (size_t i = 0; engineResult == WFM_ENGINE_OK && i < work->itemCount; ++i)
  {
    struct stat st;
    const uint64_t itemPackSize = stat(work->sources[i], &st) == 0 &&
        st.st_size > 0 ? (uint64_t)st.st_size : 0;
    pthread_mutex_lock(&gTaskMutex);
    work->currentIndex = (uint32_t)i;
    work->itemPackSize = itemPackSize;
    pthread_mutex_unlock(&gTaskMutex);
    EngineSetCurrentFile(work, work->sources[i]);
    engineResult = WfmEngineExtract(work->sources[i], work->destinations[i],
        work->password ? work->password : "", work->overwrite != 0,
        &callbacks, message, sizeof(message));
#if WFM_DEBUG
    printf("[wfm-perf] request=%llu item=%u result=%d message=%s\n",
        (unsigned long long)work->requestId, (unsigned)i,
        engineResult, message);
    fflush(stdout);
#endif
    if (engineResult != WFM_ENGINE_OK) break;
    if (work->itemCount > 1 && work->totalPackSize)
      EngineSetProgress(work, 1, 1);
    pthread_mutex_lock(&gTaskMutex);
    work->completedCount = (uint32_t)(i + 1);
    if (work->itemCount > 1 && work->totalPackSize)
    {
      work->itemBaseDone = work->done;
      work->total = work->totalPackSize;
    }
    pthread_mutex_unlock(&gTaskMutex);
  }

  pthread_mutex_lock(&gTaskMutex);
  if (engineResult == WFM_ENGINE_OK)
  {
    work->state = WFM_TASK_DONE;
    if (work->clientFd >= 0)
    {
      pthread_mutex_lock(&gSendMutex);
      (void)WfmSendFrame(work->clientFd, WFM_MSG_DONE, 0,
          work->requestId, NULL, 0);
      pthread_mutex_unlock(&gSendMutex);
    }
  }
  else
  {
    work->state = engineResult == WFM_ENGINE_CANCELED ?
        WFM_TASK_CANCELED : WFM_TASK_FAILED;
    work->errorCode = strdup(EngineErrorCode(engineResult));
    work->errorMessage = strdup(message[0] ? message :
        "Archive extraction failed");
    if (work->clientFd >= 0 && work->errorCode && work->errorMessage)
      (void)SendError(work->clientFd, work->requestId,
          work->errorCode, work->errorMessage);
  }
  if (work->password)
  {
    memset(work->password, 0, strlen(work->password));
    free(work->password);
    work->password = NULL;
  }
  pthread_mutex_unlock(&gTaskMutex);
  return NULL;
}

static int StartExtract(int fd, const struct WfmFrame *frame,
    const char *payload)
{
  struct ExtractWork *work = (struct ExtractWork *)calloc(1, sizeof(*work));
  pthread_t thread;

  if (!work)
    return SendError(fd, frame->requestId, "out_of_memory", "Out of memory");
  work->clientFd = fd;
  work->requestId = frame->requestId;
  if (ParseExtractRequest(payload, frame->payloadSize, work) != 0)
  {
    ClearExtractWork(work);
    return SendError(fd, frame->requestId,
        "invalid_request", "Invalid extraction request");
  }

  pthread_mutex_lock(&gTaskMutex);
  if(__atomic_load_n(&gStopping, __ATOMIC_RELAXED)) {
    pthread_mutex_unlock(&gTaskMutex); ClearExtractWork(work);
    return SendError(fd, frame->requestId, "stopping", "Archive helper is stopping");
  }
  unsigned retained = 0;
  for (struct ExtractWork *item = gTasks; item; item = item->next) {
    if (item->state == WFM_TASK_RUNNING) {
      pthread_mutex_unlock(&gTaskMutex);
      ClearExtractWork(work);
      return SendError(fd, frame->requestId, "busy", "An extraction is already running");
    }
    retained++;
  }
  if (retained >= 8) {
    pthread_mutex_unlock(&gTaskMutex);
    ClearExtractWork(work);
    return SendError(fd, frame->requestId, "retained_tasks", "Inspect and acknowledge retained archive tasks");
  }
  if (FindTaskLocked(frame->requestId))
  {
    pthread_mutex_unlock(&gTaskMutex);
    ClearExtractWork(work);
    return SendError(fd, frame->requestId,
        "duplicate_task_id", "Archive task ID already exists");
  }
  work->state = WFM_TASK_RUNNING;
  work->current = strdup(work->sources[0]);
  if (!work->current)
  {
    pthread_mutex_unlock(&gTaskMutex);
    ClearExtractWork(work);
    return SendError(fd, frame->requestId, "out_of_memory", "Out of memory");
  }
  work->next = gTasks;
  gTasks = work;
  pthread_mutex_unlock(&gTaskMutex);

  if (SendEmptyFrame(fd, WFM_MSG_ACCEPTED, frame->requestId) != 0 ||
      pthread_create(&thread, NULL, ExtractWorker, work) != 0)
  {
    pthread_mutex_lock(&gTaskMutex);
    for (struct ExtractWork **link = &gTasks; *link; link = &(*link)->next)
    {
      if (*link == work)
      {
        *link = work->next;
        break;
      }
    }
    pthread_mutex_unlock(&gTaskMutex);
    ClearExtractWork(work);
    return -1;
  }
  pthread_detach(thread);
  return 0;
}

static void CancelExtract(int fd, uint64_t requestId)
{
  pthread_mutex_lock(&gTaskMutex);
  struct ExtractWork *work = FindTaskLocked(requestId);
  if (work && work->clientFd == fd)
    work->cancelRequested = 1;
  pthread_mutex_unlock(&gTaskMutex);
}

static void DetachClient(int fd)
{
  pthread_mutex_lock(&gTaskMutex);
  for (struct ExtractWork *work = gTasks; work; work = work->next)
  {
    if (work->clientFd == fd)
      work->clientFd = -1;
  }
  pthread_mutex_unlock(&gTaskMutex);
}

static int SendSnapshotLocked(int fd, const struct ExtractWork *work)
{
  const size_t currentLength = work->current ? strlen(work->current) : 0;
  const size_t codeLength = work->errorCode ? strlen(work->errorCode) : 0;
  const size_t messageLength = work->errorMessage ? strlen(work->errorMessage) : 0;
  size_t size = 44 + currentLength + codeLength + messageLength;
  size_t offset;
  unsigned char *payload;
  int result;

  for (size_t i = 0; i < work->itemCount; ++i)
    size += 8 + strlen(work->sources[i]) + strlen(work->destinations[i]);
  if (size > WFM_HELPER_MAX_PAYLOAD)
    return -1;
  payload = (unsigned char *)malloc(size);
  if (!payload)
    return -1;
  WfmWrite32(payload, work->state);
  WfmWrite32(payload + 4, (uint32_t)work->itemCount);
  WfmWrite32(payload + 8, work->currentIndex);
  WfmWrite32(payload + 12, work->completedCount);
  WfmWrite64(payload + 16, work->total);
  WfmWrite64(payload + 24, work->done);
  WfmWrite32(payload + 32, (uint32_t)currentLength);
  WfmWrite32(payload + 36, (uint32_t)codeLength);
  WfmWrite32(payload + 40, (uint32_t)messageLength);
  offset = 44;
  memcpy(payload + offset, work->current ? work->current : "", currentLength);
  offset += currentLength;
  memcpy(payload + offset, work->errorCode ? work->errorCode : "", codeLength);
  offset += codeLength;
  memcpy(payload + offset, work->errorMessage ? work->errorMessage : "",
      messageLength);
  offset += messageLength;
  for (size_t i = 0; i < work->itemCount; ++i)
  {
    const size_t sourceLength = strlen(work->sources[i]);
    const size_t destinationLength = strlen(work->destinations[i]);
    WfmWrite32(payload + offset, (uint32_t)sourceLength);
    WfmWrite32(payload + offset + 4, (uint32_t)destinationLength);
    offset += 8;
    memcpy(payload + offset, work->sources[i], sourceLength);
    offset += sourceLength;
    memcpy(payload + offset, work->destinations[i], destinationLength);
    offset += destinationLength;
  }
  pthread_mutex_lock(&gSendMutex);
  result = WfmSendFrame(fd, WFM_MSG_TASK_SNAPSHOT, 0, work->requestId,
      payload, (uint32_t)size);
  pthread_mutex_unlock(&gSendMutex);
  free(payload);
  return result;
}

static int ListTasks(int fd, uint64_t requestId)
{
  int result = 0;
  pthread_mutex_lock(&gTaskMutex);
  for (struct ExtractWork *work = gTasks; work; work = work->next)
  {
    if (SendSnapshotLocked(fd, work) != 0)
    {
      result = -1;
      break;
    }
  }
  pthread_mutex_unlock(&gTaskMutex);
  if (!result)
    result = SendEmptyFrame(fd, WFM_MSG_LIST_DONE, requestId);
  return result;
}

static int AttachTask(int fd, uint64_t requestId)
{
  int result = 0;
  pthread_mutex_lock(&gTaskMutex);
  struct ExtractWork *work = FindTaskLocked(requestId);
  if (work)
  {
    work->clientFd = fd;
    result = SendSnapshotLocked(fd, work);
  }
  pthread_mutex_unlock(&gTaskMutex);
  return work ? result : SendError(fd, requestId,
      "task_not_found", "Archive task was not found");
}

static int AcknowledgeTask(int fd, uint64_t requestId)
{
  struct ExtractWork *removed = NULL;
  pthread_mutex_lock(&gTaskMutex);
  for (struct ExtractWork **link = &gTasks; *link; link = &(*link)->next)
  {
    if ((*link)->requestId == requestId &&
        (*link)->state != WFM_TASK_RUNNING)
    {
      removed = *link;
      *link = removed->next;
      break;
    }
  }
  pthread_mutex_unlock(&gTaskMutex);
  if (!removed)
    return SendError(fd, requestId, "task_not_finished",
        "Archive task is still running or was not found");
  ClearExtractWork(removed);
  return SendEmptyFrame(fd, WFM_MSG_DONE, requestId);
}

static int HandleClientFrames(int fd, char *payload)
{
  struct WfmFrame frame;

  for (;;)
  {
    const int result = WfmRecvFrame(fd, &frame, payload,
        WFM_HELPER_MAX_PAYLOAD + 1U);
    if (result <= 0)
      return result;

    if (frame.flags != 0)
      return SendError(fd, frame.requestId,
          "invalid_request", "Unsupported protocol flags");

    switch (frame.type)
    {
      case WFM_MSG_PING:
        if (frame.payloadSize != 0)
          return SendError(fd, frame.requestId,
              "invalid_request", "Invalid ping request");
        pthread_mutex_lock(&gSendMutex);
        {
          const char version[] = WFM_HELPER_VERSION;
          const int sent = WfmSendFrame(fd, WFM_MSG_PONG, 0, frame.requestId,
                                        version, sizeof(version)-1);
          pthread_mutex_unlock(&gSendMutex);
          if(sent != 0) return -1;
        }
        break;

      case WFM_MSG_BFPILOT_SHUTDOWN:
        if(frame.payloadSize != 0) return -1;
        pthread_mutex_lock(&gTaskMutex);
        for(struct ExtractWork *work = gTasks; work; work = work->next) {
          if(work->state == WFM_TASK_RUNNING) {
            pthread_mutex_unlock(&gTaskMutex);
            return SendError(fd, frame.requestId, "busy", "Cannot stop an active extraction");
          }
        }
        __atomic_store_n(&gStopping, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&gTaskMutex);
        (void)SendEmptyFrame(fd, WFM_MSG_BFPILOT_STOPPED, frame.requestId);
        return 0;

      case WFM_MSG_EXTRACT:
        if (StartExtract(fd, &frame, payload) != 0)
          return -1;
        break;

      case WFM_MSG_CANCEL:
        if (frame.payloadSize != 0)
          return SendError(fd, frame.requestId,
              "invalid_request", "Invalid cancel request");
        CancelExtract(fd, frame.requestId);
        break;

      case WFM_MSG_LIST_TASKS:
        if (frame.payloadSize != 0 || ListTasks(fd, frame.requestId) != 0)
          return -1;
        break;

      case WFM_MSG_ATTACH_TASK:
        if (frame.payloadSize != 0 || AttachTask(fd, frame.requestId) != 0)
          return -1;
        break;

      case WFM_MSG_ACK_TASK:
        if (frame.payloadSize != 0 || AcknowledgeTask(fd, frame.requestId) != 0)
          return -1;
        break;

      default:
        if (SendError(fd, frame.requestId,
            "unknown_message", "Unknown helper message") != 0)
          return -1;
        break;
    }
  }
}

static int HandleClient(int fd)
{
  char *payload = (char *)malloc(WFM_HELPER_MAX_PAYLOAD + 1U);
  int result;
  if (!payload)
    return -1;
  result = HandleClientFrames(fd, payload);
  free(payload);
  return result;
}

static void *ClientWorker(void *argument)
{
  const int fd = (int)(intptr_t)argument;
  (void)HandleClient(fd);
  DetachClient(fd);
  close(fd);
  return NULL;
}

int main(void)
{
  struct sigaction action;
  int lockFd;

  SetProcessName();

  signal(SIGPIPE, SIG_IGN);
  memset(&action, 0, sizeof(action));
  action.sa_handler = StopHandler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);

  lockFd = AcquireInstanceLock();
  if (lockFd < 0)
  {
    if (errno == EWOULDBLOCK)
      fprintf(stderr, "bfpilot-7zip is already running\n");
    else
      fprintf(stderr, "helper lock failed: %s\n", strerror(errno));
    return errno == EWOULDBLOCK ? 0 : 1;
  }

  gServerFd = CreateServer();
  if (gServerFd < 0)
  {
    fprintf(stderr, "helper socket failed: %s\n", strerror(errno));
    close(lockFd);
    return 1;
  }

#if WFM_DEBUG
  printf("bfpilot-7zip v%s listening on %s\n",
      WFM_HELPER_VERSION, WFM_HELPER_SOCKET);
#endif
  /* Embedded service startup is reported by the coordinator UI. */

  while (!__atomic_load_n(&gStopping, __ATOMIC_RELAXED))
  {
    struct pollfd ready = {gServerFd, POLLIN, 0};
    const int event = poll(&ready, 1, 250);
    if(event == 0 || (event < 0 && errno == EINTR)) continue;
    if(event < 0 || !(ready.revents & POLLIN)) break;
    const int clientFd = accept(gServerFd, NULL, NULL);
    if (clientFd < 0)
    {
      if (errno == EINTR)
        continue;
      if (!__atomic_load_n(&gStopping, __ATOMIC_RELAXED))
        fprintf(stderr, "accept failed: %s\n", strerror(errno));
      break;
    }
    pthread_t clientThread;
    if (pthread_create(&clientThread, NULL, ClientWorker,
        (void *)(intptr_t)clientFd) != 0)
    {
      close(clientFd);
      continue;
    }
    pthread_detach(clientThread);
  }

  for(;;) {
    bool running = false;
    pthread_mutex_lock(&gTaskMutex);
    for(struct ExtractWork *work = gTasks; work; work = work->next)
      if(work->state == WFM_TASK_RUNNING) running = true;
    pthread_mutex_unlock(&gTaskMutex);
    if(!running) break;
    usleep(20000);
  }
  if (gServerFd >= 0)
    close(gServerFd);
  gServerFd = -1;
  unlink(WFM_HELPER_SOCKET);
  close(lockFd);
  return 0;
}
