// WfmProtocol.cpp

#include "WfmProtocol.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const unsigned char kMagic[4] = { 'B', '7', 'H', 'P' };

static uint16_t Get16(const unsigned char *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t WfmRead32(const unsigned char *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) | p[3];
}

uint64_t WfmRead64(const unsigned char *p)
{
  return ((uint64_t)WfmRead32(p) << 32) | WfmRead32(p + 4);
}

static void Put16(unsigned char *p, uint16_t value)
{
  p[0] = (unsigned char)(value >> 8);
  p[1] = (unsigned char)value;
}

void WfmWrite32(unsigned char *p, uint32_t value)
{
  p[0] = (unsigned char)(value >> 24);
  p[1] = (unsigned char)(value >> 16);
  p[2] = (unsigned char)(value >> 8);
  p[3] = (unsigned char)value;
}

void WfmWrite64(unsigned char *p, uint64_t value)
{
  WfmWrite32(p, (uint32_t)(value >> 32));
  WfmWrite32(p + 4, (uint32_t)value);
}

static int RecvAll(int fd, void *buffer, size_t size)
{
  unsigned char *p = (unsigned char *)buffer;
  size_t done = 0;

  while (done < size)
  {
    const ssize_t n = recv(fd, p + done, size - done, 0);
    if (n == 0)
      return 0;
    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      return -1;
    }
    done += (size_t)n;
  }
  return 1;
}

static int SendAll(int fd, const void *buffer, size_t size)
{
  const unsigned char *p = (const unsigned char *)buffer;
  size_t done = 0;

  while (done < size)
  {
    const ssize_t n = send(fd, p + done, size - done, MSG_NOSIGNAL);
    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
    {
      errno = EPIPE;
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

int WfmRecvFrame(int fd, struct WfmFrame *frame, char *payload,
    size_t payloadCapacity)
{
  unsigned char header[WFM_HELPER_HEADER_SIZE];
  int result;

  if (!frame || (!payload && payloadCapacity != 0))
  {
    errno = EINVAL;
    return -1;
  }

  result = RecvAll(fd, header, sizeof(header));
  if (result <= 0)
    return result;

  if (memcmp(header, kMagic, sizeof(kMagic)) != 0)
  {
    errno = EPROTO;
    return -1;
  }

  frame->type = Get16(header + 4);
  frame->flags = Get16(header + 6);
  frame->payloadSize = WfmRead32(header + 8);
  frame->requestId = WfmRead64(header + 12);

  if (frame->payloadSize > WFM_HELPER_MAX_PAYLOAD ||
      frame->payloadSize >= payloadCapacity)
  {
    errno = EMSGSIZE;
    return -1;
  }

  if (frame->payloadSize != 0)
  {
    result = RecvAll(fd, payload, frame->payloadSize);
    if (result <= 0)
      return result == 0 ? -1 : result;
  }
  if (payload && payloadCapacity != 0)
    payload[frame->payloadSize] = 0;
  return 1;
}

int WfmSendFrame(int fd, uint16_t type, uint32_t flags, uint64_t requestId,
    const void *payload, uint32_t payloadSize)
{
  unsigned char header[WFM_HELPER_HEADER_SIZE];

  if (flags > 0xffffU || payloadSize > WFM_HELPER_MAX_PAYLOAD ||
      (payloadSize && !payload))
  {
    errno = EINVAL;
    return -1;
  }

  memcpy(header, kMagic, sizeof(kMagic));
  Put16(header + 4, type);
  Put16(header + 6, (uint16_t)flags);
  WfmWrite32(header + 8, payloadSize);
  WfmWrite64(header + 12, requestId);

  if (SendAll(fd, header, sizeof(header)) != 0)
    return -1;
  if (payloadSize && SendAll(fd, payload, payloadSize) != 0)
    return -1;
  return 0;
}
