#ifndef WFM_READ_AHEAD_H
#define WFM_READ_AHEAD_H
#include <pthread.h>
#include "../../../Common/MyCom.h"
#include "../../IStream.h"
#include "WfmPerf.h"

class WfmReadAheadStream Z7_final:
  public IInStream, public IStreamGetSize, public IStreamGetProps,
  public IStreamGetProps2, public IStreamGetProp, public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_6(IInStream, ISequentialInStream, IStreamGetSize,
      IStreamGetProps, IStreamGetProps2, IStreamGetProp)
  Z7_IFACE_COM7_IMP(ISequentialInStream)
  Z7_IFACE_COM7_IMP(IInStream)
  Z7_IFACE_COM7_IMP(IStreamGetSize)
  Z7_IFACE_COM7_IMP(IStreamGetProps)
  Z7_IFACE_COM7_IMP(IStreamGetProps2)
  Z7_IFACE_COM7_IMP(IStreamGetProp)
  CMyComPtr<IInStream> _source;
  Byte *_buffers[2];
  UInt32 _sizes[2], _offset;
  bool _ready[2], _started, _stop, _done, _enabled, _disabled, _terminal;
  unsigned _readIndex, _fillIndex;
  UInt64 _position;
  HRESULT _result;
  pthread_t _thread;
  pthread_mutex_t _mutex;
  pthread_cond_t _cond;
  WfmPerfStats *_perf;
  static void *ReaderMain(void *arg);
  void RunReader();
  bool StartReader();
  void StopReader();
  void FreeBuffers();
public:
  explicit WfmReadAheadStream(IInStream *source);
  ~WfmReadAheadStream();
  void EnablePrefetch(WfmPerfStats *perf = NULL) { _enabled = true; _perf = perf; }
};
#endif
