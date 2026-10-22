#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include "../EngineGuids.cpp"
#include "../WfmReadAhead.h"
static bool failThread;
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p)
{ return failThread ? EAGAIN : __real_pthread_create(t, a, f, p); }
class Source Z7_final: public IInStream, public IStreamGetSize, public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IInStream, ISequentialInStream, IStreamGetSize)
  Z7_IFACE_COM7_IMP(ISequentialInStream)
  Z7_IFACE_COM7_IMP(IInStream)
  Z7_IFACE_COM7_IMP(IStreamGetSize)
public:
  std::vector<Byte> bytes;
  UInt64 pos, errorAt;
  UInt32 maxRead;
  bool slow;
  explicit Source(size_t size): bytes(size), pos(0), errorAt((UInt64)-1),
      maxRead(1u << 21), slow(false)
  { for (size_t i = 0; i < size; ++i) bytes[i] = (Byte)(i * 31 + i / 257); }
};
Z7_COM7F_IMF(Source::Read(void *data, UInt32 size, UInt32 *got))
{
  *got = 0;
  if (slow) usleep(1000);
  if (pos >= errorAt) return E_FAIL;
  if (pos >= bytes.size()) return S_OK;
  if (size > maxRead) size = maxRead;
  if (size > bytes.size() - pos) size = (UInt32)(bytes.size() - pos);
  if (size > errorAt - pos) size = (UInt32)(errorAt - pos);
  memcpy(data, bytes.data() + pos, size);
  pos += size; *got = size;
  return S_OK;
}
Z7_COM7F_IMF(Source::Seek(Int64 offset, UInt32 origin, UInt64 *where))
{
  if (origin > STREAM_SEEK_END) return E_INVALIDARG;
  Int64 base = origin == STREAM_SEEK_CUR ? (Int64)pos :
      origin == STREAM_SEEK_END ? (Int64)bytes.size() : 0;
  if (offset < -base) return E_FAIL;
  pos = (UInt64)(base + offset);
  if (where) *where = pos;
  return S_OK;
}
Z7_COM7F_IMF(Source::GetSize(UInt64 *size)) { *size = bytes.size(); return S_OK; }
static void Check(IInStream *stream, const Source &source, UInt64 &pos, UInt32 size)
{
  std::vector<Byte> data(size ? size : 1);
  UInt32 got = 123;
  assert(stream->Read(data.data(), size, &got) == S_OK && got <= size);
  if (size && pos < source.bytes.size()) assert(got > 0);
  assert(got == 0 || memcmp(data.data(), source.bytes.data() + pos, got) == 0);
  pos += got;
}
int main()
{
  for (unsigned mode = 0; mode < 3; ++mode)
  {
    failThread = mode == 2;
    Source *source = new Source(6 * (1u << 20) + 123);
    if (mode == 1) source->maxRead = 70001;
    CMyComPtr<IInStream> original = source;
    WfmReadAheadStream *reader = new WfmReadAheadStream(original);
    CMyComPtr<IInStream> stream = reader; reader->EnablePrefetch();
    UInt64 pos = 0, where;
    Check(stream, *source, pos, 0); Check(stream, *source, pos, 12345);
    assert(stream->Seek(0, STREAM_SEEK_CUR, &where) == S_OK && where == pos);
    assert(stream->Seek(-37, STREAM_SEEK_CUR, &where) == S_OK);
    pos -= 37; assert(where == pos);
    Check(stream, *source, pos, 3u << 20);
    assert(stream->Seek(57, STREAM_SEEK_SET, &where) == S_OK); pos = 57;
    while (pos < source->bytes.size()) Check(stream, *source, pos, 262147);
    Check(stream, *source, pos, 1); Check(stream, *source, pos, 1);
    assert(stream->Seek(-53, STREAM_SEEK_END, &where) == S_OK);
    pos = source->bytes.size() - 53; Check(stream, *source, pos, 100);
    assert(stream->Seek(-1, STREAM_SEEK_SET, NULL) != S_OK);
    Check(stream, *source, pos, 1);
    CMyComPtr<IStreamGetSize> prop;
    assert(stream->QueryInterface(IID_IStreamGetSize, (void **)&prop) == S_OK);
    assert(prop->GetSize(&where) == S_OK && where == source->bytes.size());
  }
  failThread = false;
  {
    // Automatic activation must also work when debug printing is disabled.
    WfmPerfStats perf("automatic-prefetch-test");
    perf.BeginExtract();
    Source *source = new Source(6u << 20);
    CMyComPtr<IInStream> original = source;
    CMyComPtr<IInStream> stream = new WfmReadAheadStream(original);
    UInt64 pos = 0;
    Check(stream, *source, pos, 1024);
    assert(source->pos == pos && perf.inputCopyBytes.load() == 0);
    Check(stream, *source, pos, 1u << 20);
    assert(perf.inputCopyBytes.load() != 0);
    assert(stream->Seek(17, STREAM_SEEK_SET, NULL) == S_OK);
    pos = 17;
    const uint64_t copied = perf.inputCopyBytes.load();
    Check(stream, *source, pos, 1024);
    assert(source->pos == pos && perf.inputCopyBytes.load() == copied);
  }
  {
    WfmPerfStats perf("threshold-test");
    Source *source = new Source(6u << 20);
    CMyComPtr<IInStream> original = source;
    WfmReadAheadStream *reader = new WfmReadAheadStream(original);
    CMyComPtr<IInStream> stream = reader;
    reader->EnablePrefetch(&perf);
    UInt64 pos = 0;
    Check(stream, *source, pos, 1024);
    assert(source->pos == pos && perf.inputCopyBytes.load() == 0);
    Check(stream, *source, pos, 1u << 20);
    assert(perf.inputCopyBytes.load() != 0);
    assert(stream->Seek(17, STREAM_SEEK_SET, NULL) == S_OK);
    pos = 17;
    const uint64_t copied = perf.inputCopyBytes.load();
    Check(stream, *source, pos, 1024);
    assert(source->pos == pos && perf.inputCopyBytes.load() == copied);
  }
  {
    Source *source = new Source(3u << 20); source->errorAt = 100000;
    CMyComPtr<IInStream> original = source;
    WfmReadAheadStream *reader = new WfmReadAheadStream(original);
    CMyComPtr<IInStream> stream = reader; reader->EnablePrefetch();
    UInt64 pos = 0; while (pos < 100000) Check(stream, *source, pos, 4093);
    Byte data; UInt32 got = 1;
    assert(stream->Read(&data, 1, &got) == E_FAIL && got == 0);
    assert(stream->Read(&data, 1, &got) == E_FAIL && got == 0);
    assert(stream->Seek(0, STREAM_SEEK_SET, NULL) == S_OK);
    pos = 0; Check(stream, *source, pos, 4093);
  }
  for (unsigned i = 0; i < 20; ++i)
  {
    Source *source = new Source(5u << 20); source->slow = true;
    CMyComPtr<IInStream> original = source;
    WfmReadAheadStream *reader = new WfmReadAheadStream(original);
    CMyComPtr<IInStream> stream = reader; reader->EnablePrefetch();
    UInt64 pos = 0; Check(stream, *source, pos, 17);
  }
  puts("read ahead: bytes/tail/short/EOF/relative-seek/end-seek/error/restart/fallback/shutdown/props passed");
}
