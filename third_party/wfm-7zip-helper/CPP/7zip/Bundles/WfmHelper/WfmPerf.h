#ifndef WFM_PERF_H
#define WFM_PERF_H
#include <atomic>
#include <stdint.h>
#ifndef WFM_DEBUG
#ifdef WFM_PLATFORM_PS5
#define WFM_DEBUG 0
#else
#define WFM_DEBUG 1
#endif
#endif
uint64_t WfmPerfNow();
uint64_t WfmPerfThreadCpu();
class WfmPerfStats
{
  WfmPerfStats *_previous;
  uint64_t _id, _start, _last, _cpuStart, _processStart;
#if WFM_DEBUG
  uint64_t _lastRead, _lastWrite, _lastReadNs, _lastWriteNs;
  uint64_t _lastWaitNs, _lastCopyNs;
#endif
  bool _active;
public:
  std::atomic<uint64_t> readBytes, readNs, readCalls;
  std::atomic<uint64_t> writeBytes, writeNs, writeCalls, writerCpuNs;
  std::atomic<uint64_t> waitNs, copyNs, copyBytes;
  std::atomic<uint64_t> prefetchWaitNs, inputCopyNs, inputCopyBytes;
  explicit WfmPerfStats(const char *source);
  ~WfmPerfStats();
  void BeginExtract();
  void Report(bool final = false);
  bool Active() const { return _active; }
};
WfmPerfStats *WfmPerfCurrent();
void WfmPerfRecordRead(uint64_t bytes, uint64_t ns);
#endif
