// Exercise the actual output stream with injected short writes and failures.
#include <assert.h>
#include <vector>
#include "../EngineGuids.cpp"
#include "../WfmEngine.cpp"

static bool failThread;
static int writeMode;
static unsigned writeCalls;
static unsigned threadsStarted;
static unsigned allocationCalls, failAllocation;
extern "C" void *__real_malloc(size_t);
extern "C" void *__wrap_malloc(size_t size)
{
  if (++allocationCalls == failAllocation) { errno = ENOMEM; return NULL; }
  return __real_malloc(size);
}
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *,
    void *(*)(void *), void *);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __wrap_pthread_create(pthread_t *thread,
    const pthread_attr_t *attr, void *(*entry)(void *), void *context)
{
  if (failThread) return EAGAIN;
  const int result = __real_pthread_create(thread, attr, entry, context);
  if (!result) ++threadsStarted;
  return result;
}
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size)
{
  if (writeMode == 2) return 0;
  if (writeMode == 3) usleep(2000);
  if (writeMode == 4 && ++writeCalls >= 2)
    { errno = ENOSPC; return -1; }
  if (writeMode == 1)
  {
    if (++writeCalls % 4 == 0) { errno = EINTR; return -1; }
    if (size > 4093) size = 4093;
  }
  return __real_write(fd, data, size);
}
static void Complete(int parent, const std::vector<Byte> &data,
    bool buffered, UInt32 step = 262147)
{
  const int fd = openat(parent, "part", O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0);
  {
    WfmOutStream out(fd, dup(parent), "part", "final", NULL, buffered);
    size_t offset = 0;
    while (offset < data.size())
    {
      const UInt32 count = (UInt32)((data.size() - offset > step) ?
          step : data.size() - offset);
      UInt32 done = 0;
      assert(static_cast<ISequentialOutStream &>(out).Write(
          data.data() + offset, count, &done) == S_OK);
      assert(done > 0 && done <= count);
      offset += done;
    }
    assert(out.Commit());
    assert(!out.NoSpace());
  }
  const int result = openat(parent, "final", O_RDONLY);
  assert(result >= 0);
  std::vector<Byte> actual(data.size() + 1);
  size_t size = 0;
  ssize_t count;
  while ((count = read(result, actual.data() + size, actual.size() - size)) > 0)
    size += (size_t)count;
  assert(count == 0 && size == data.size());
  assert(memcmp(actual.data(), data.data(), data.size()) == 0);
  close(result);
  assert(faccessat(parent, "part", F_OK, 0) != 0);
  assert(unlinkat(parent, "final", 0) == 0);
}
static int Canceled(void *context) { return *(bool *)context; }
int main()
{
  assert(!WfmUseOutputPipeline(false, (UInt64)-1));
  assert(!WfmUseOutputPipeline(true, kWfmBufferedFileSize - 1));
  assert(WfmUseOutputPipeline(true, kWfmBufferedFileSize));
  assert(WfmUseOutputPipeline(true, kWfmBufferedFileSize + 1));
  char directory[] = "/tmp/wfm-outstream-test-XXXXXX";
  assert(mkdtemp(directory));
  const int parent = open(directory, O_RDONLY | O_DIRECTORY);
  assert(parent >= 0);
  std::vector<Byte> data((9u << 20) + 123);
  for (size_t i = 0; i < data.size(); ++i) data[i] = (Byte)(i * 37 + i / 97);
  for (unsigned i = 0; i < 8; ++i) Complete(parent, data, true);
  assert(threadsStarted == 8);
  Complete(parent, data, true, 5u << 20);
  std::vector<Byte> queuedData((25u << 20) + 127);
  for (size_t i = 0; i < queuedData.size(); ++i)
    queuedData[i] = (Byte)(i * 13 + i / 193);
  writeMode = 3;
  Complete(parent, queuedData, true, 5u << 20);
  writeMode = 0;
  writeMode = 1;
  Complete(parent, data, true);
  Complete(parent, data, true, 5u << 20);
  writeMode = 0;
  Complete(parent, data, false);
  failThread = true;
  Complete(parent, data, true);
  failThread = false;
  for (unsigned i = 1; i <= 2; ++i)
  {
    allocationCalls = 0;
    failAllocation = i;
    const unsigned before = threadsStarted;
    Complete(parent, data, true);
    assert(threadsStarted == before);
  }
  failAllocation = 0;
  writeMode = 4;
  writeCalls = 0;
  {
    const int fd = openat(parent, "part", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    WfmOutStream out(fd, dup(parent), "part", "final", NULL, true);
    UInt32 done = 0;
    assert(static_cast<ISequentialOutStream &>(out).Write(
        queuedData.data(), (UInt32)queuedData.size(), &done) == E_FAIL);
    assert(!out.Commit() && out.NoSpace());
  }
  writeMode = 0;
  assert(faccessat(parent, "part", F_OK, 0) != 0);
  assert(faccessat(parent, "final", F_OK, 0) != 0);
  bool canceled = false;
  WfmEngineCallbacks callbacks = { &canceled, Canceled, NULL, NULL };
  {
    const int fd = openat(parent, "part", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    WfmOutStream out(fd, dup(parent), "part", "final", &callbacks, true);
    UInt32 done = 0;
    assert(static_cast<ISequentialOutStream &>(out).Write(
        queuedData.data(), 21u << 20, &done) == S_OK);
    canceled = true;
    assert(static_cast<ISequentialOutStream &>(out).Write(
        data.data(), 1, &done) == E_ABORT && done == 0);
  }
  assert(faccessat(parent, "part", F_OK, 0) != 0);
  assert(faccessat(parent, "final", F_OK, 0) != 0);
  {
    const int fd = open("/dev/full", O_WRONLY);
    assert(fd >= 0);
    WfmOutStream out(fd, dup(parent), "missing", "final", NULL, true);
    UInt32 done = 0;
    const HRESULT result = static_cast<ISequentialOutStream &>(out).Write(
        data.data(), (4u << 20) + 123, &done);
    assert(result == S_OK);
    assert(!out.Commit() && out.NoSpace());
  }
  writeMode = 2;
  {
    const int fd = openat(parent, "part", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    WfmOutStream out(fd, dup(parent), "part", "final", NULL, true);
    UInt32 done = 0;
    assert(static_cast<ISequentialOutStream &>(out).Write(
        data.data(), 123, &done) == S_OK);
    assert(!out.Commit() && !out.NoSpace());
  }
  writeMode = 0;
  assert(faccessat(parent, "part", F_OK, 0) != 0);
  assert(faccessat(parent, "final", F_OK, 0) != 0);
  close(parent);
  assert(rmdir(directory) == 0);
  puts("output stream: threshold/queue/tail/partial/EINTR/allocation/thread fallback/cancel/late-ENOSPC/zero-write passed");
}
