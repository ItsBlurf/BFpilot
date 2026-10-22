// WfmProtocol.h
// Wire protocol shared by wfm-7zip-helper and ps5-web-file-mgr.

#ifndef ZIP7_INC_WFM_HELPER_PROTOCOL_H
#define ZIP7_INC_WFM_HELPER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define WFM_HELPER_HEADER_SIZE 20U
#define WFM_HELPER_MAX_PAYLOAD (1024U * 1024U)
#define WFM_HELPER_MAX_PATH (256U * 1024U)
#define WFM_HELPER_MAX_PASSWORD (256U * 1024U)

/* The wire header is encoded explicitly; it is not a native C structure. */
enum WfmMessageType
{
  WFM_MSG_PING = 1,
  WFM_MSG_PONG = 2,
  WFM_MSG_EXTRACT = 10,
  WFM_MSG_CANCEL = 11,
  WFM_MSG_PASSWORD = 12,
  WFM_MSG_LIST_TASKS = 13,
  WFM_MSG_ATTACH_TASK = 14,
  WFM_MSG_ACK_TASK = 15,
  WFM_MSG_ACCEPTED = 20,
  WFM_MSG_PROGRESS = 21,
  WFM_MSG_CURRENT_FILE = 22,
  WFM_MSG_PASSWORD_REQUIRED = 23,
  WFM_MSG_DONE = 24,
  WFM_MSG_ERROR = 25,
  WFM_MSG_TASK_SNAPSHOT = 26,
  WFM_MSG_LIST_DONE = 27,
  WFM_MSG_BFPILOT_SHUTDOWN = 100,
  WFM_MSG_BFPILOT_STOPPED = 101
};

struct WfmFrame
{
  uint16_t type;
  uint32_t flags;
  uint64_t requestId;
  uint32_t payloadSize;
};

/* Returns 1 on success, 0 on orderly disconnect, and -1 on error. */
int WfmRecvFrame(int fd, struct WfmFrame *frame, char *payload,
    size_t payloadCapacity);

/* Returns 0 on success and -1 on error. */
int WfmSendFrame(int fd, uint16_t type, uint32_t flags, uint64_t requestId,
    const void *payload, uint32_t payloadSize);

uint32_t WfmRead32(const unsigned char *p);
uint64_t WfmRead64(const unsigned char *p);
void WfmWrite32(unsigned char *p, uint32_t value);
void WfmWrite64(unsigned char *p, uint64_t value);

#endif
