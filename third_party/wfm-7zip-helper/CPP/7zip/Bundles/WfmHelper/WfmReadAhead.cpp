#include "WfmReadAhead.h"
#include <stdlib.h>
#include <string.h>
static const UInt32 kReadAheadSize = 1u << 21;
static const UInt32 kMinPrefetchRead = 1u << 20;

WfmReadAheadStream::WfmReadAheadStream(IInStream *source): _source(source),
    _offset(0), _started(false), _stop(false), _done(false), _enabled(false),
    _disabled(false), _terminal(false), _readIndex(0), _fillIndex(0),
    _position(0), _result(S_OK), _perf(NULL)
{
  _buffers[0] = _buffers[1] = NULL;
  _sizes[0] = _sizes[1] = 0;
  _ready[0] = _ready[1] = false;
  _source->Seek(0, STREAM_SEEK_CUR, &_position);
}
WfmReadAheadStream::~WfmReadAheadStream()
{ StopReader(); FreeBuffers(); }
void WfmReadAheadStream::FreeBuffers()
{
  free(_buffers[0]); free(_buffers[1]);
  _buffers[0] = _buffers[1] = NULL;
}
bool WfmReadAheadStream::StartReader()
{
  if (_disabled) return false;
  if (!_buffers[0]) _buffers[0] = (Byte *)malloc(kReadAheadSize);
  if (!_buffers[1]) _buffers[1] = (Byte *)malloc(kReadAheadSize);
  if (_buffers[0] && _buffers[1] && pthread_mutex_init(&_mutex, NULL) == 0)
  {
    if (pthread_cond_init(&_cond, NULL) == 0)
    {
      _stop = _done = false;
      _readIndex = _fillIndex = _offset = 0;
      _ready[0] = _ready[1] = false;
      _result = S_OK;
      _started = pthread_create(&_thread, NULL, ReaderMain, this) == 0;
      if (!_started) pthread_cond_destroy(&_cond);
    }
    if (!_started) pthread_mutex_destroy(&_mutex);
  }
  if (!_started) { _disabled = true; FreeBuffers(); }
  return _started;
}
void WfmReadAheadStream::StopReader()
{
  if (!_started) return;
  pthread_mutex_lock(&_mutex);
  _stop = true;
  pthread_cond_broadcast(&_cond);
  pthread_mutex_unlock(&_mutex);
  pthread_join(_thread, NULL);
  pthread_cond_destroy(&_cond);
  pthread_mutex_destroy(&_mutex);
  _started = false;
}
void *WfmReadAheadStream::ReaderMain(void *arg)
{ ((WfmReadAheadStream *)arg)->RunReader(); return NULL; }
void WfmReadAheadStream::RunReader()
{
  pthread_mutex_lock(&_mutex);
  for (;;)
  {
    while (_ready[_fillIndex] && !_stop) pthread_cond_wait(&_cond, &_mutex);
    if (_stop) break;
    const unsigned index = _fillIndex;
    pthread_mutex_unlock(&_mutex);
    UInt32 got = 0;
    const uint64_t start = _perf ? WfmPerfNow() : 0;
    const HRESULT result = _source->Read(_buffers[index], kReadAheadSize, &got);
    if (_perf)
    {
      _perf->readNs.fetch_add(WfmPerfNow() - start);
      _perf->readBytes.fetch_add(got);
      _perf->readCalls.fetch_add(1);
    }
    pthread_mutex_lock(&_mutex);
    _sizes[index] = got;
    _ready[index] = got != 0;
    _fillIndex ^= 1;
    if (result != S_OK || got == 0) { _done = true; _result = result; }
    pthread_cond_broadcast(&_cond);
    if (_done) break;
  }
  pthread_mutex_unlock(&_mutex);
}
Z7_COM7F_IMF(WfmReadAheadStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize) *processedSize = 0;
  if (!size) return S_OK;
  if (!_enabled && size >= kMinPrefetchRead)
    if (WfmPerfStats *perf = WfmPerfCurrent()) EnablePrefetch(perf);
  if (_terminal) return _result;
  // After a seek, small metadata/member reads should not start speculative I/O.
  if (!_enabled || (_perf && !_started && size < kMinPrefetchRead) ||
      (!_started && !StartReader()))
  {
    UInt32 got = 0;
    const HRESULT result = _source->Read(data, size, &got);
    _position += got;
    if (processedSize) *processedSize = got;
    return result;
  }
  pthread_mutex_lock(&_mutex);
  const uint64_t waitStart = _perf ? WfmPerfNow() : 0;
  while (!_ready[_readIndex] && !_done) pthread_cond_wait(&_cond, &_mutex);
  if (_perf) _perf->prefetchWaitNs.fetch_add(WfmPerfNow() - waitStart);
  if (!_ready[_readIndex])
  {
    const HRESULT result = _result;
    _terminal = true;
    pthread_mutex_unlock(&_mutex);
    StopReader(); FreeBuffers();
    return result;
  }
  UInt32 got = _sizes[_readIndex] - _offset;
  if (got > size) got = size;
  const uint64_t copyStart = _perf ? WfmPerfNow() : 0;
  memcpy(data, _buffers[_readIndex] + _offset, got);
  if (_perf)
  {
    _perf->inputCopyNs.fetch_add(WfmPerfNow() - copyStart);
    _perf->inputCopyBytes.fetch_add(got);
  }
  _offset += got;
  _position += got;
  if (_offset == _sizes[_readIndex])
  {
    _ready[_readIndex] = false;
    _readIndex ^= 1;
    _offset = 0;
    pthread_cond_signal(&_cond);
  }
  pthread_mutex_unlock(&_mutex);
  if (processedSize) *processedSize = got;
  if (_perf) _perf->Report();
  return S_OK;
}
Z7_COM7F_IMF(WfmReadAheadStream::Seek(Int64 offset, UInt32 origin, UInt64 *newPosition))
{
  if ((origin == STREAM_SEEK_CUR && offset == 0) ||
      (origin == STREAM_SEEK_SET && offset >= 0 && (UInt64)offset == _position))
  {
    if (newPosition) *newPosition = _position;
    return S_OK;
  }
  StopReader();
  _terminal = false;
  // The physical source may be ahead: relative seeks use the consumed position.
  RINOK(_source->Seek((Int64)_position, STREAM_SEEK_SET, NULL))
  UInt64 position = 0;
  const HRESULT result = _source->Seek(offset, origin, &position);
  if (result == S_OK) _position = position;
  if (result == S_OK && newPosition) *newPosition = position;
  return result;
}
Z7_COM7F_IMF(WfmReadAheadStream::GetSize(UInt64 *size))
{
  CMyComPtr<IStreamGetSize> prop;
  RINOK(_source->QueryInterface(IID_IStreamGetSize, (void **)&prop))
  return prop->GetSize(size);
}
Z7_COM7F_IMF(WfmReadAheadStream::GetProps(UInt64 *size, FILETIME *c, FILETIME *a, FILETIME *m, UInt32 *attr))
{
  CMyComPtr<IStreamGetProps> prop;
  RINOK(_source->QueryInterface(IID_IStreamGetProps, (void **)&prop))
  return prop->GetProps(size, c, a, m, attr);
}
Z7_COM7F_IMF(WfmReadAheadStream::GetProps2(CStreamFileProps *props))
{
  CMyComPtr<IStreamGetProps2> prop;
  RINOK(_source->QueryInterface(IID_IStreamGetProps2, (void **)&prop))
  return prop->GetProps2(props);
}
Z7_COM7F_IMF(WfmReadAheadStream::GetProperty(PROPID id, PROPVARIANT *value))
{
  CMyComPtr<IStreamGetProp> prop;
  RINOK(_source->QueryInterface(IID_IStreamGetProp, (void **)&prop))
  return prop->GetProperty(id, value);
}
Z7_COM7F_IMF(WfmReadAheadStream::ReloadProps())
{
  CMyComPtr<IStreamGetProp> prop;
  RINOK(_source->QueryInterface(IID_IStreamGetProp, (void **)&prop))
  return prop->ReloadProps();
}
