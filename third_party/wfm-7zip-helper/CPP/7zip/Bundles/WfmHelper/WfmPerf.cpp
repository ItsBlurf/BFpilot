#include "WfmPerf.h"
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>
#include "../../../../C/Aes.h"
static thread_local WfmPerfStats *g_current;
static std::atomic<uint64_t> g_nextId(0);
uint64_t WfmPerfNow()
{
  struct timespec v;
  return clock_gettime(CLOCK_MONOTONIC, &v) == 0 ?
      (uint64_t)v.tv_sec * 1000000000ULL + v.tv_nsec : 0;
}
uint64_t WfmPerfThreadCpu()
{
  struct timespec v;
  return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &v) == 0 ?
      (uint64_t)v.tv_sec * 1000000000ULL + v.tv_nsec : 0;
}
static uint64_t ProcessCpu()
{
  struct rusage v;
  if (getrusage(RUSAGE_SELF, &v) != 0) return 0;
  return ((uint64_t)v.ru_utime.tv_sec + v.ru_stime.tv_sec) *
      1000000000ULL + ((uint64_t)v.ru_utime.tv_usec + v.ru_stime.tv_usec) * 1000ULL;
}
WfmPerfStats *WfmPerfCurrent()
{ return g_current && g_current->Active() ? g_current : NULL; }
WfmPerfStats::WfmPerfStats(const char *source):
    _previous(g_current), _id(++g_nextId), _start(0), _last(0),
    _cpuStart(0), _processStart(0),
#if WFM_DEBUG
    _lastRead(0), _lastWrite(0),
    _lastReadNs(0), _lastWriteNs(0), _lastWaitNs(0), _lastCopyNs(0),
#endif
    _active(false), readBytes(0), readNs(0), readCalls(0),
    writeBytes(0), writeNs(0), writeCalls(0), writerCpuNs(0),
    waitNs(0), copyNs(0), copyBytes(0), prefetchWaitNs(0), inputCopyNs(0), inputCopyBytes(0)
{
  g_current = this;
#if WFM_DEBUG
  printf("[wfm-perf] id=%llu opening source=%s\n", (unsigned long long)_id, source);
  fflush(stdout);
#else
  (void)source;
#endif
}
WfmPerfStats::~WfmPerfStats()
{
  if (_active) Report(true);
  g_current = _previous;
}
void WfmPerfStats::BeginExtract()
{
  _start = _last = WfmPerfNow();
  _cpuStart = WfmPerfThreadCpu();
  _processStart = ProcessCpu();
  _active = true;
#if WFM_DEBUG
#ifdef WFM_RAR_LARGE_OUTPUT
  printf("[wfm-perf] id=%llu rar5_output_step=4MiB input_prefetch=2x2MiB (large reads only)\n",
      (unsigned long long)_id);
#endif
  const char *aes = g_AesCbc_Decode == AesCbc_Decode ? "software" :
      g_AesCbc_Decode == AesCbc_Decode_HW ? "AES-NI" : "hardware-other";
  printf("[wfm-perf] id=%llu extracting aes_cbc=%s aes_flags=0x%x "
      "thread_cpu_clock=%s large_output_buffers=2x4MiB interval=2s "
      "rates=MiB/s cpu=cumulative process_cpu=all-helper-tasks\n",
      (unsigned long long)_id, aes, (unsigned)g_Aes_SupportedFunctions_Flags,
      _cpuStart ? "available" : "unavailable");
  fflush(stdout);
#endif
}
#if WFM_DEBUG
static double Rate(uint64_t bytes, uint64_t ns)
{ return ns ? (double)bytes * 1e9 / ns / (1024.0 * 1024.0) : 0; }
#endif
void WfmPerfStats::Report(bool final)
{
#if WFM_DEBUG
  const uint64_t now = WfmPerfNow();
  if (!_active || now <= _last || (!final && now - _last < 2000000000ULL)) return;
  const uint64_t elapsed = now - _start, interval = now - _last;
  const uint64_t r = readBytes.load(), w = writeBytes.load();
  const uint64_t rn = readNs.load(), wn = writeNs.load();
  const uint64_t wait = waitNs.load(), copy = copyNs.load();
  const uint64_t cpu = WfmPerfThreadCpu(), process = ProcessCpu();
  const double threadPercent = _cpuStart && cpu >= _cpuStart ?
      (double)(cpu - _cpuStart) * 100 / elapsed : 0;
  const double processPercent = process >= _processStart ?
      (double)(process - _processStart) * 100 / elapsed : 0;
  printf("[wfm-perf] id=%llu %s elapsed=%.2fs in=%.3fGiB out=%.3fGiB "
      "out_avg=%.2f out_recent=%.2f in_recent=%.2f "
      "read_call_rate=%.2f write_call_rate=%.2f "
      "read_ms=%.1f write_ms=%.1f writer_wait_ms=%.1f copy_ms=%.1f "
      "recent_read=%.1f%% recent_writer_wait=%.1f%% recent_copy=%.1f%% "
      "extract_thread_cpu=%.1f%% write_call_cpu=%.1f%% process_cpu=%.1f%% "
      "read_calls=%llu write_calls=%llu copied=%.3fGiB in_bytes=%llu out_bytes=%llu "
      "prefetch_wait_ms=%.1f input_copy_ms=%.1f input_copied=%.3fGiB\n",
      (unsigned long long)_id, final ? "final" : "sample",
      (double)elapsed / 1e9, (double)r / (1ULL << 30), (double)w / (1ULL << 30),
      Rate(w, elapsed), Rate(w - _lastWrite, interval), Rate(r - _lastRead, interval),
      Rate(r - _lastRead, rn - _lastReadNs), Rate(w - _lastWrite, wn - _lastWriteNs),
      (double)rn / 1e6, (double)wn / 1e6, (double)wait / 1e6, (double)copy / 1e6,
      (double)(rn - _lastReadNs) * 100 / interval,
      (double)(wait - _lastWaitNs) * 100 / interval,
      (double)(copy - _lastCopyNs) * 100 / interval, threadPercent,
      (double)writerCpuNs.load() * 100 / elapsed, processPercent,
      (unsigned long long)readCalls.load(), (unsigned long long)writeCalls.load(),
      (double)copyBytes.load() / (1ULL << 30),
      (unsigned long long)r, (unsigned long long)w,
      (double)prefetchWaitNs.load() / 1e6, (double)inputCopyNs.load() / 1e6,
      (double)inputCopyBytes.load() / (1ULL << 30));
  fflush(stdout);
  _last = now; _lastRead = r; _lastWrite = w;
  _lastReadNs = rn; _lastWriteNs = wn; _lastWaitNs = wait; _lastCopyNs = copy;
#else
  (void)final;
#endif
}
void WfmPerfRecordRead(uint64_t bytes, uint64_t ns)
{
  WfmPerfStats *perf = WfmPerfCurrent();
  if (!perf) return;
  perf->readBytes.fetch_add(bytes);
  perf->readNs.fetch_add(ns);
  perf->readCalls.fetch_add(1);
  perf->Report();
}
