#include "WfmEngine.h"
#include "WfmPerf.h"
#include "WfmReadAhead.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../../Common/MyCom.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/UTFConvert.h"
#include "../../../Windows/PropVariant.h"
#include "../../../Windows/TimeUtils.h"
#include "../../Archive/IArchive.h"
#include "../../Common/FileStreams.h"
#include "../../IPassword.h"
#include "../../UI/Common/LoadCodecs.h"
#include "BfpilotFsAt.h"

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static int SetFdTimes(int fd, const CFiTime times[2])
{
#ifdef __linux__
  return futimens(fd, times);
#else
  struct timeval values[2];
  values[0].tv_sec = times[0].tv_sec;
  values[0].tv_usec = times[0].tv_nsec / 1000;
  values[1].tv_sec = times[1].tv_sec;
  values[1].tv_usec = times[1].tv_nsec / 1000;
  return futimes(fd, values);
#endif
}

static void SetMessage(char *message, unsigned int size, const char *text)
{
  if (message && size)
    snprintf(message, size, "%s", text ? text : "");
}

static bool IsCanceled(const WfmEngineCallbacks *callbacks)
{
  return callbacks && callbacks->isCanceled &&
      callbacks->isCanceled(callbacks->context);
}

static bool EndsWithNoCase(const char *text, const char *suffix)
{
  const size_t textLength = strlen(text);
  const size_t suffixLength = strlen(suffix);
  return textLength >= suffixLength &&
      strcasecmp(text + textLength - suffixLength, suffix) == 0;
}

static bool IsCompoundTarSource(const char *source)
{
  return EndsWithNoCase(source, ".tar.gz") || EndsWithNoCase(source, ".tgz") ||
      EndsWithNoCase(source, ".tar.bz2") || EndsWithNoCase(source, ".tbz") ||
      EndsWithNoCase(source, ".tbz2") || EndsWithNoCase(source, ".tar.xz") ||
      EndsWithNoCase(source, ".txz") || EndsWithNoCase(source, ".tar.zst") ||
      EndsWithNoCase(source, ".tzst");
}

static bool Utf8ToUnicode(const char *src, UString &dest)
{
  AString value(src);
  return ConvertUTF8ToUnicode(value, dest);
}

static bool NormalizeMemberPath(const UString &path, AString &normalized)
{
  AString utf8;
  ConvertUnicodeToUTF8(path, utf8);
  normalized.Empty();
  if (utf8.IsEmpty() || utf8[0] == '/' || utf8[0] == '\\' ||
      (utf8.Len() >= 2 && utf8[1] == ':'))
    return false;
  AString component;
  for (unsigned i = 0; i <= utf8.Len(); ++i)
  {
    const char c = i == utf8.Len() ? '/' : utf8[i];
    if (c == '/' || c == '\\')
    {
      if (component == "." || component == "..")
        return false;
      if (!component.IsEmpty())
      {
        if (!normalized.IsEmpty()) normalized += '/';
        normalized += component;
        component.Empty();
      }
    }
    else
    {
      if ((unsigned char)c < 0x20 || component.Len() >= 255) return false;
      component += c;
    }
  }
  return !normalized.IsEmpty() && normalized.Len() < 4096;
}

static int OpenDirectoryTreeAt(int baseFd, const char *path, bool create,
    bool chmodEntries, bool *unsafe)
{
  char copy[4096];
  char *save = NULL;
  char *part;
  int fd = openat(baseFd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0 || strlen(path) >= sizeof(copy))
  {
    if (fd >= 0) close(fd);
    return -1;
  }
  strcpy(copy, path);
  for (part = strtok_r(copy, "/", &save); part;
       part = strtok_r(NULL, "/", &save))
  {
    if (!strcmp(part, ".")) continue;
    if (!strcmp(part, "..") || !part[0])
    {
      *unsafe = true;
      close(fd);
      return -1;
    }
    struct stat st;
    bool createdNow = false;
    if (fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) != 0)
    {
      /* Some PS5 libc filesystem wrappers don't update errno. Attempt the
         atomic relative mkdir, then inspect the entry instead of trusting it. */
      if (!create)
      {
        close(fd);
        return -1;
      }
      createdNow = mkdirat(fd, part, 0777) == 0;
      if (fstatat(fd, part, &st, AT_SYMLINK_NOFOLLOW) != 0)
      {
        close(fd);
        return -1;
      }
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
    {
      *unsafe = true;
      close(fd);
      return -1;
    }
    const int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (next < 0)
    {
      if (errno == ELOOP || errno == ENOTDIR) *unsafe = true;
      close(fd);
      return -1;
    }
    if ((createdNow || chmodEntries) && fchmod(next, 0777) != 0)
    {
      close(next);
      close(fd);
      return -1;
    }
    close(fd);
    fd = next;
  }
  return fd;
}

static int OpenRootDirectory(const char *path, bool *unsafe)
{
  const bool absolute = path[0] == '/';
  int base = open(absolute ? "/" : ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (base < 0) return -1;
  while (*path == '/') ++path;
  const int result = OpenDirectoryTreeAt(base, path, true, false, unsafe);
  close(base);
  return result;
}

static int OpenMemberParent(int rootFd, const AString &member,
    char *name, size_t nameSize, bool *unsafe)
{
  char path[4096];
  if (member.Len() >= sizeof(path)) return -1;
  strcpy(path, member.Ptr());
  char *slash = strrchr(path, '/');
  const char *leaf = path;
  int parent;
  if (slash)
  {
    *slash = 0;
    leaf = slash + 1;
    parent = OpenDirectoryTreeAt(rootFd, path, true, true, unsafe);
  }
  else
    parent = openat(rootFd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (parent < 0 || !leaf[0] || strlen(leaf) >= nameSize)
  {
    if (parent >= 0) close(parent);
    return -1;
  }
  strcpy(name, leaf);
  return parent;
}

static void ChmodDirectoryTreeFd(int fd)
{
  (void)fchmod(fd, 0777);
  const int scanFd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (scanFd < 0) return;
  DIR *dir = fdopendir(scanFd);
  if (!dir) { close(scanFd); return; }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
  {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    struct stat st;
    if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        S_ISLNK(st.st_mode)) continue;
    if (S_ISDIR(st.st_mode))
    {
      const int child = openat(fd, entry->d_name,
          O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (child >= 0) { ChmodDirectoryTreeFd(child); close(child); }
    }
    else if (S_ISREG(st.st_mode))
    {
      const int file = openat(fd, entry->d_name, O_RDONLY | O_NOFOLLOW);
      if (file >= 0) { (void)fchmod(file, 0777); close(file); }
    }
  }
  closedir(dir);
}

static void SetMemberDirectoryTimeAt(int rootFd, const AString &member,
    const CFiTime &mtime)
{
  bool unsafe = false;
  const int fd = OpenDirectoryTreeAt(rootFd, member.Ptr(), false, false,
      &unsafe);
  if (fd < 0) return;
  struct stat st;
  CFiTime times[2];
  if (fstat(fd, &st) == 0)
  {
    times[0] = ST_ATIME(st);
    times[1] = mtime;
    (void)SetFdTimes(fd, times);
  }
  close(fd);
}

static void ChmodMemberTreeAt(int rootFd, const AString &member)
{
  bool unsafe = false;
  const int fd = OpenDirectoryTreeAt(rootFd, member.Ptr(), false, false,
      &unsafe);
  if (fd < 0) return;
  ChmodDirectoryTreeFd(fd);
  close(fd);
}

class WfmScopedFd
{
  int _fd;
public:
  explicit WfmScopedFd(int fd = -1): _fd(fd) {}
  ~WfmScopedFd() { if (_fd >= 0) close(_fd); }
  int Get() const { return _fd; }
};

static const size_t kWfmWriteBufferSize = (size_t)1 << 22;
static const UInt64 kWfmBufferedFileSize = (UInt64)32 << 20;

static bool WfmUseOutputPipeline(bool sizeDefined, UInt64 size)
{ return sizeDefined && size >= kWfmBufferedFileSize; }

static int WriteAll(int fd, const Byte *data, size_t size,
    WfmPerfStats *perf)
{
  size_t offset = 0;
  while (offset < size)
  {
    const uint64_t start = perf ? WfmPerfNow() : 0;
    const uint64_t cpuStart = perf ? WfmPerfThreadCpu() : 0;
    const ssize_t written = write(fd, data + offset, size - offset);
    const int writeErrno = errno;
    if (perf)
    {
      perf->writeNs.fetch_add(WfmPerfNow() - start);
      const uint64_t cpu = WfmPerfThreadCpu();
      if (cpuStart && cpu >= cpuStart) perf->writerCpuNs.fetch_add(cpu - cpuStart);
      if (written > 0) perf->writeBytes.fetch_add((uint64_t)written);
      perf->writeCalls.fetch_add(1);
    }
    errno = writeErrno;
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return written < 0 ? errno : EIO;
    offset += (size_t)written;
  }
  return 0;
}

class WfmOutStream Z7_final: public ISequentialOutStream, public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
  Z7_IFACE_COM7_IMP(ISequentialOutStream)
  int _fd;
  int _parentFd;
  char _tempName[96];
  char _finalName[256];
  bool _committed;
  bool _noSpace;
  bool _writeError;
  // The producer fills _buffer while the worker owns _writeBuffer.
  Byte *_buffer;
  Byte *_writeBuffer;
  size_t _bufferSize;
  size_t _pendingSize;
  bool _pending;
  bool _writing;
  bool _stop;
  bool _async;
  pthread_t _writer;
  pthread_mutex_t _writeMutex;
  pthread_cond_t _writeCond;
  bool _hasMTime;
  CFiTime _mTime;
  const WfmEngineCallbacks *_callbacks;
  WfmPerfStats *_perf;
public:
  WfmOutStream(int fd, int parentFd, const char *tempName,
      const char *finalName, const WfmEngineCallbacks *callbacks,
      bool buffered):
      _fd(fd), _parentFd(parentFd), _committed(false),
      _noSpace(false), _writeError(false),
      _buffer(buffered ? (Byte *)malloc(kWfmWriteBufferSize) : NULL),
      _writeBuffer(_buffer ? (Byte *)malloc(kWfmWriteBufferSize) : NULL),
      _bufferSize(0), _pendingSize(0), _pending(false), _writing(false),
      _stop(false), _async(false), _hasMTime(false), _callbacks(callbacks),
      _perf(WfmPerfCurrent())
  {
    snprintf(_tempName, sizeof(_tempName), "%s", tempName);
    snprintf(_finalName, sizeof(_finalName), "%s", finalName);
    if (_writeBuffer)
    {
      if (pthread_mutex_init(&_writeMutex, NULL) == 0)
      {
        if (pthread_cond_init(&_writeCond, NULL) == 0)
        {
          _async = pthread_create(&_writer, NULL, WriterMain, this) == 0;
          if (!_async) pthread_cond_destroy(&_writeCond);
        }
        if (!_async) pthread_mutex_destroy(&_writeMutex);
      }
      if (!_async)
      {
        free(_writeBuffer);
        _writeBuffer = NULL;
      }
    }
  }
  ~WfmOutStream()
  {
    if (_async) StopWriter(false);
    free(_writeBuffer);
    free(_buffer);
    if (_fd >= 0) close(_fd);
    if (!_committed && _parentFd >= 0) unlinkat(_parentFd, _tempName, 0);
    if (_parentFd >= 0) close(_parentFd);
  }
  bool Commit()
  {
    // All accepted bytes must finish writing before publishing this member.
    bool flushed = Flush();
    if (_async && !StopWriter(flushed)) flushed = false;
    if (!flushed || _writeError) return false;
    if (_fd >= 0)
    {
      if (fchmod(_fd, 0777) != 0) return false;
      if (_hasMTime)
      {
        struct stat st;
        CFiTime times[2];
        if (fstat(_fd, &st) != 0) return false;
        times[0] = ST_ATIME(st);
        times[1] = _mTime;
        if (SetFdTimes(_fd, times) != 0) return false;
      }
      close(_fd);
      _fd = -1;
    }
    if (renameat(_parentFd, _tempName, _parentFd, _finalName) != 0)
      return false;
    _committed = true;
    return true;
  }
  void SetMTime(const FILETIME &value)
    { _hasMTime = FILETIME_To_timespec(value, _mTime); }
  bool NoSpace()
  {
    if (_async) pthread_mutex_lock(&_writeMutex);
    const bool result = _noSpace;
    if (_async) pthread_mutex_unlock(&_writeMutex);
    return result;
  }
private:
  static void *WriterMain(void *context)
  {
    WfmOutStream *self = (WfmOutStream *)context;
    self->RunWriter();
    return NULL;
  }
  void RunWriter()
  {
    pthread_mutex_lock(&_writeMutex);
    for (;;)
    {
      while (!_pending && !_stop)
        pthread_cond_wait(&_writeCond, &_writeMutex);
      if (_stop && !_pending) break;
      const Byte *data = _writeBuffer;
      const size_t size = _pendingSize;
      _pending = false;
      _writing = true;
      pthread_mutex_unlock(&_writeMutex);
      const int error = WriteAll(_fd, data, size, _perf);
      pthread_mutex_lock(&_writeMutex);
      _writing = false;
      if (error)
      {
        _writeError = true;
        if (error == ENOSPC || error == EDQUOT) _noSpace = true;
      }
      pthread_cond_broadcast(&_writeCond);
    }
    pthread_mutex_unlock(&_writeMutex);
  }
  bool StopWriter(bool drain)
  {
    pthread_mutex_lock(&_writeMutex);
    if (!drain) _pending = false;
    const uint64_t waitStart = _perf ? WfmPerfNow() : 0;
    while (_writing || (drain && _pending))
      pthread_cond_wait(&_writeCond, &_writeMutex);
    if (_perf) _perf->waitNs.fetch_add(WfmPerfNow() - waitStart);
    const bool result = !_writeError;
    _stop = true;
    pthread_cond_signal(&_writeCond);
    pthread_mutex_unlock(&_writeMutex);
    pthread_join(_writer, NULL);
    pthread_cond_destroy(&_writeCond);
    pthread_mutex_destroy(&_writeMutex);
    _async = false;
    return result;
  }
  bool HasWriteError()
  {
    if (_async) pthread_mutex_lock(&_writeMutex);
    const bool result = _writeError;
    if (_async) pthread_mutex_unlock(&_writeMutex);
    return result;
  }
  bool Flush()
  {
    if (_bufferSize == 0) return true;
    if (_async)
    {
      pthread_mutex_lock(&_writeMutex);
      const uint64_t waitStart = _perf ? WfmPerfNow() : 0;
      while (_pending || _writing)
        pthread_cond_wait(&_writeCond, &_writeMutex);
      if (_perf) _perf->waitNs.fetch_add(WfmPerfNow() - waitStart);
      if (_writeError)
      {
        pthread_mutex_unlock(&_writeMutex);
        return false;
      }
      Byte *next = _writeBuffer;
      _writeBuffer = _buffer;
      _buffer = next;
      _pendingSize = _bufferSize;
      _bufferSize = 0;
      _pending = true;
      pthread_cond_signal(&_writeCond);
      pthread_mutex_unlock(&_writeMutex);
      return true;
    }
    const int error = WriteAll(_fd, _buffer, _bufferSize, _perf);
    if (error)
    {
      _writeError = true;
      if (error == ENOSPC || error == EDQUOT) _noSpace = true;
      return false;
    }
    _bufferSize = 0;
    return true;
  }
};

static WfmOutStream *CreateOutputStreamAt(int rootFd, const AString &member,
    bool overwrite, bool buffered, const WfmEngineCallbacks *callbacks,
    bool *conflict, bool *unsafe)
{
  static volatile unsigned long counter;
  char name[256];
  const int parent = OpenMemberParent(rootFd, member, name, sizeof(name),
      unsafe);
  if (parent < 0) return NULL;
  struct stat st;
  const bool existed = fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
  if (existed)
  {
    if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
      *unsafe = true;
    else if (!overwrite)
      *conflict = true;
    if (*unsafe || *conflict)
    {
      close(parent);
      return NULL;
    }
  }
  char temp[96];
  int fd = -1;
  for (unsigned attempt = 0; attempt < 32 && fd < 0; ++attempt)
  {
    const unsigned long value = __sync_add_and_fetch(&counter, 1);
    snprintf(temp, sizeof(temp), ".wfm-part-%lx-%lx", (unsigned long)getpid(),
        value);
    fd = openat(parent, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0700);
  }
  if (fd < 0)
  {
    close(parent);
    return NULL;
  }
  return new WfmOutStream(fd, parent, temp, name, callbacks, buffered);
}

Z7_COM7F_IMF(WfmOutStream::Write(const void *data, UInt32 size,
    UInt32 *processedSize))
{
  ssize_t written;
  if (processedSize) *processedSize = 0;
  if (IsCanceled(_callbacks)) return E_ABORT;
  if (HasWriteError()) return E_FAIL;
  if (_buffer)
  {
    const Byte *bytes = (const Byte *)data;
    UInt32 accepted = 0;
    while (accepted < size)
    {
      if (_bufferSize == kWfmWriteBufferSize && !Flush()) return E_FAIL;
      size_t count = kWfmWriteBufferSize - _bufferSize;
      if (count > size - accepted) count = size - accepted;
      const uint64_t copyStart = _perf ? WfmPerfNow() : 0;
      memcpy(_buffer + _bufferSize, bytes + accepted, count);
      if (_perf)
      {
        _perf->copyNs.fetch_add(WfmPerfNow() - copyStart);
        _perf->copyBytes.fetch_add(count);
      }
      _bufferSize += count;
      accepted += (UInt32)count;
    }
    if (processedSize) *processedSize = accepted;
    return S_OK;
  }
  do
  {
    const uint64_t start = _perf ? WfmPerfNow() : 0;
    written = write(_fd, data, size);
    const int writeErrno = errno;
    if (_perf)
    {
      _perf->writeNs.fetch_add(WfmPerfNow() - start);
      if (written > 0) _perf->writeBytes.fetch_add((uint64_t)written);
      _perf->writeCalls.fetch_add(1);
    }
    errno = writeErrno;
  } while (written < 0 && errno == EINTR);
  if (written < 0)
  {
    if (errno == ENOSPC || errno == EDQUOT) _noSpace = true;
    return E_FAIL;
  }
  if (processedSize) *processedSize = (UInt32)written;
  return S_OK;
}

class WfmOpenCallback Z7_final:
  public IArchiveOpenCallback,
  public IArchiveOpenVolumeCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(IArchiveOpenCallback, IArchiveOpenVolumeCallback,
      ICryptoGetTextPassword)
public:
  UString Password;
  UString ArchiveName;
  AString SourceDirectory;
  AString MissingVolume;
  bool PasswordRequested;
  const WfmEngineCallbacks *Callbacks;
  explicit WfmOpenCallback(const WfmEngineCallbacks *callbacks):
      PasswordRequested(false), Callbacks(callbacks) {}
};

Z7_COM7F_IMF(WfmOpenCallback::SetTotal(const UInt64 *, const UInt64 *))
  { return IsCanceled(Callbacks) ? E_ABORT : S_OK; }
Z7_COM7F_IMF(WfmOpenCallback::SetCompleted(const UInt64 *, const UInt64 *))
  { return IsCanceled(Callbacks) ? E_ABORT : S_OK; }
Z7_COM7F_IMF(WfmOpenCallback::GetProperty(PROPID propID, PROPVARIANT *value))
{
  NWindows::NCOM::CPropVariant prop;
  if (propID == kpidName) prop = ArchiveName;
  prop.Detach(value);
  return S_OK;
}
Z7_COM7F_IMF(WfmOpenCallback::GetStream(const wchar_t *name,
    IInStream **inStream))
{
  *inStream = NULL;
  UString nameU(name);
  AString nameUtf8;
  ConvertUnicodeToUTF8(nameU, nameUtf8);
  if (nameUtf8.IsEmpty() || strchr(nameUtf8.Ptr(), '/') ||
      strchr(nameUtf8.Ptr(), '\\') || nameUtf8 == "." || nameUtf8 == "..")
    return S_FALSE;
  AString full = SourceDirectory;
  full += nameUtf8;
  UString fullU;
  if (!ConvertUTF8ToUnicode(full, fullU)) return S_FALSE;
  CInFileStream *streamSpec = new CInFileStream;
  CMyComPtr<IInStream> stream = streamSpec;
  if (!streamSpec->Open(us2fs(fullU)))
  {
    MissingVolume = nameUtf8;
    return S_FALSE;
  }
  CMyComPtr<IInStream> prefetched = new WfmReadAheadStream(stream);
  *inStream = prefetched.Detach();
  return S_OK;
}
Z7_COM7F_IMF(WfmOpenCallback::CryptoGetTextPassword(BSTR *password))
{
  PasswordRequested = true;
  if (Password.IsEmpty()) return E_ABORT;
  return StringToBstr(Password, password);
}

static HRESULT GetMemberPath(IInArchive *archive, UInt32 index,
    const AString &defaultName, AString &path, bool &valid)
{
  NWindows::NCOM::CPropVariant prop;
  valid = false;
  const HRESULT result = archive->GetProperty(index, kpidPath, &prop);
  if (result != S_OK) return result;
  if (prop.vt == VT_EMPTY)
  {
    path = defaultName;
    valid = !path.IsEmpty();
  }
  else if (prop.vt == VT_BSTR)
  {
    UString member(prop.bstrVal);
    valid = NormalizeMemberPath(member, path);
  }
  return S_OK;
}

static bool MemberIsSkippedNode(IInArchive *archive, UInt32 index)
{
  const PROPID linkProps[] = { kpidSymLink, kpidHardLink };
  for (unsigned p = 0; p < 2; ++p)
  {
    NWindows::NCOM::CPropVariant prop;
    if (archive->GetProperty(index, linkProps[p], &prop) == S_OK &&
        ((prop.vt == VT_BSTR && prop.bstrVal && prop.bstrVal[0]) ||
         (prop.vt != VT_EMPTY && prop.vt != VT_BSTR)))
      return true;
  }
  NWindows::NCOM::CPropVariant mode;
  if (archive->GetProperty(index, kpidPosixAttrib, &mode) == S_OK &&
      mode.vt == VT_UI4)
  {
    const mode_t type = (mode_t)(mode.ulVal & S_IFMT);
    if (type && type != S_IFREG && type != S_IFDIR) return true;
  }
  return false;
}

static HRESULT GetMemberIsDirectory(IInArchive *archive, UInt32 index,
    bool &isDirectory)
{
  NWindows::NCOM::CPropVariant prop;
  const HRESULT result = archive->GetProperty(index, kpidIsDir, &prop);
  if (result != S_OK) return result;
  isDirectory = prop.vt == VT_BOOL && VARIANT_BOOLToBool(prop.boolVal);
  return S_OK;
}

static void AddUniqueString(CObjectVector<AString> &items,
    const AString &value)
{
  FOR_VECTOR (i, items) if (items[i] == value) return;
  items.Add(value);
}

class WfmExtractCallback Z7_final:
  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
  CMyComPtr<IInArchive> _archive;
  CMyComPtr<ISequentialOutStream> _stream;
  WfmOutStream *_streamSpec;
  int _rootFd;
  const WfmEngineCallbacks *_callbacks;
  bool _overwrite;
  AString _defaultName;
  CObjectVector<AString> _topDirectories;
  CObjectVector<AString> _directoryTimePaths;
  CRecordVector<CFiTime> _directoryTimes;
  UInt64 _progressDone;
  UInt64 _progressTotal;
public:
  UString Password;
  bool PasswordRequested;
  bool UnsafePath;
  bool OverwriteRequired;
  bool NoSpace;
  bool IoError;
  Int32 OperationResult;
  WfmExtractCallback(IInArchive *archive, const char *source,
      int rootFd, bool overwrite, UInt64 packSize,
      const WfmEngineCallbacks *callbacks):
      _archive(archive), _streamSpec(NULL),
      _rootFd(openat(rootFd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW)),
      _callbacks(callbacks), _overwrite(overwrite),
      _defaultName(strrchr(source, '/') ? strrchr(source, '/') + 1 : source),
      _progressDone(0), _progressTotal(packSize),
      PasswordRequested(false), UnsafePath(false), OverwriteRequired(false),
      NoSpace(false), IoError(false),
      OperationResult(NArchive::NExtract::NOperationResult::kOK)
  {
    const char *tarAliases[] = { ".tgz", ".tbz", ".tbz2", ".txz", ".tzst" };
    bool tarAlias = false;
    for (unsigned i = 0; i < sizeof(tarAliases) / sizeof(tarAliases[0]); ++i)
    {
      const unsigned extLength = (unsigned)strlen(tarAliases[i]);
      if (_defaultName.Len() > extLength && EndsWithNoCase(_defaultName.Ptr(), tarAliases[i]))
      {
        _defaultName.DeleteFrom(_defaultName.Len() - extLength);
        _defaultName += ".tar";
        tarAlias = true;
        break;
      }
    }
    if (!tarAlias)
    {
      const char *extensions[] = { ".bz2", ".gzip", ".gz", ".lzma", ".xz", ".zst", ".zstd" };
      for (unsigned i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
      {
        const unsigned extLength = (unsigned)strlen(extensions[i]);
        if (_defaultName.Len() > extLength && EndsWithNoCase(_defaultName.Ptr(), extensions[i]))
        {
          _defaultName.DeleteFrom(_defaultName.Len() - extLength);
          break;
        }
      }
    }
    if (_defaultName.IsEmpty()) _defaultName = "Content";
  }
  ~WfmExtractCallback() { if (_rootFd >= 0) close(_rootFd); }
  void AbortCurrentOutput()
  {
    _stream.Release();
    _streamSpec = NULL;
    _topDirectories.Clear();
    _directoryTimePaths.Clear();
    _directoryTimes.Clear();
  }
  void FinishOutputs()
  {
    FOR_VECTOR (i, _topDirectories) ChmodMemberTreeAt(_rootFd,
        _topDirectories[i]);
    for (unsigned i = _directoryTimePaths.Size(); i != 0; --i)
      SetMemberDirectoryTimeAt(_rootFd, _directoryTimePaths[i - 1],
          _directoryTimes[i - 1]);
    _topDirectories.Clear();
    _directoryTimePaths.Clear();
    _directoryTimes.Clear();
  }
  bool CurrentStreamNoSpace() const
    { return _streamSpec && _streamSpec->NoSpace(); }
  const AString &DefaultName() const { return _defaultName; }
};

Z7_COM7F_IMF(WfmExtractCallback::SetTotal(UInt64 total))
{
  _progressTotal = total;
  if (_callbacks && _callbacks->setProgress)
    _callbacks->setProgress(_callbacks->context, _progressDone, _progressTotal);
  return S_OK;
}

Z7_COM7F_IMF(WfmExtractCallback::SetCompleted(const UInt64 *value))
{
  if (WfmPerfStats *perf = WfmPerfCurrent()) perf->Report();
  if (IsCanceled(_callbacks)) return E_ABORT;
  if (value)
  {
    _progressDone = *value;
    if (_callbacks && _callbacks->setProgress)
      _callbacks->setProgress(_callbacks->context, _progressDone,
          _progressTotal);
  }
  return S_OK;
}

Z7_COM7F_IMF(WfmExtractCallback::GetStream(UInt32 index,
    ISequentialOutStream **outStream, Int32 askExtractMode))
{
  *outStream = NULL;
  _stream.Release();
  _streamSpec = NULL;
  if (IsCanceled(_callbacks)) return E_ABORT;

  AString normalized;
  bool validPath;
  RINOK(GetMemberPath(_archive, index, _defaultName, normalized, validPath))
  if (!validPath)
  {
    UnsafePath = true;
    return E_ABORT;
  }
  if (MemberIsSkippedNode(_archive, index)) return S_OK;
  if (_callbacks && _callbacks->setCurrentFile)
    _callbacks->setCurrentFile(_callbacks->context, normalized.Ptr());
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;

  bool isDir;
  RINOK(GetMemberIsDirectory(_archive, index, isDir))
  const int firstSlash = normalized.Find('/');
  if (isDir || firstSlash >= 0)
  {
    AString top(normalized);
    if (firstSlash >= 0) top.DeleteFrom((unsigned)firstSlash);
    AddUniqueString(_topDirectories, top);
  }
  if (isDir)
  {
    NWindows::NCOM::CPropVariant mtimeProp;
    CFiTime mtime;
    if (_archive->GetProperty(index, kpidMTime, &mtimeProp) == S_OK &&
        mtimeProp.vt == VT_FILETIME &&
        FILETIME_To_timespec(mtimeProp.filetime, mtime))
    {
      _directoryTimePaths.Add(normalized);
      _directoryTimes.Add(mtime);
    }
    bool unsafe = false;
    const int fd = OpenDirectoryTreeAt(_rootFd, normalized.Ptr(), true, true,
        &unsafe);
    if (fd < 0)
    {
      if (unsafe) return S_OK;
      IoError = true;
      return E_ABORT;
    }
    close(fd);
    return S_OK;
  }
  NWindows::NCOM::CPropVariant sizeProp;
  struct statvfs space;
  if (_archive->GetProperty(index, kpidSize, &sizeProp) == S_OK &&
      sizeProp.vt == VT_UI8 && fstatvfs(_rootFd, &space) == 0 &&
      sizeProp.uhVal.QuadPart > (UInt64)space.f_bavail * space.f_frsize)
  {
    NoSpace = true;
    return E_ABORT;
  }
  bool unsafe = false;
  WfmOutStream *streamSpec = CreateOutputStreamAt(_rootFd, normalized,
      _overwrite, WfmUseOutputPipeline(sizeProp.vt == VT_UI8,
      sizeProp.vt == VT_UI8 ? sizeProp.uhVal.QuadPart : 0), _callbacks,
      &OverwriteRequired, &unsafe);
  if (!streamSpec)
  {
    if (unsafe) return S_OK;
    IoError = true;
    return E_ABORT;
  }
  NWindows::NCOM::CPropVariant mtimeProp;
  if (_archive->GetProperty(index, kpidMTime, &mtimeProp) == S_OK &&
      mtimeProp.vt == VT_FILETIME)
    streamSpec->SetMTime(mtimeProp.filetime);
  _streamSpec = streamSpec;
  CMyComPtr<ISequentialOutStream> stream = streamSpec;
  _stream = stream;
  *outStream = stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(WfmExtractCallback::PrepareOperation(Int32)) { return S_OK; }
Z7_COM7F_IMF(WfmExtractCallback::SetOperationResult(Int32 result))
{
  if (result == NArchive::NExtract::NOperationResult::kOK && _streamSpec)
  {
    if (!_streamSpec->Commit()) IoError = true;
  }
  else if (result != NArchive::NExtract::NOperationResult::kOK)
    OperationResult = result;
  if (_streamSpec && _streamSpec->NoSpace()) NoSpace = true;
  _stream.Release();
  _streamSpec = NULL;
  return (IoError || OverwriteRequired) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(WfmExtractCallback::CryptoGetTextPassword(BSTR *password))
{
  PasswordRequested = true;
  if (Password.IsEmpty()) return E_ABORT;
  return StringToBstr(Password, password);
}

static bool InspectArchiveItems(IInArchive *archive, int rootFd,
    const AString &defaultName, const WfmEngineCallbacks *callbacks,
    AString &conflicts, UInt64 &unpackSize, bool &unpackSizeDefined)
{
  UInt32 numItems = 0;
  CObjectVector<AString> names;
  unpackSize = 0;
  unpackSizeDefined = true;
  if (archive->GetNumberOfItems(&numItems) != S_OK)
  {
    unpackSizeDefined = false;
    return false;
  }
  for (UInt32 i = 0; i < numItems; ++i)
  {
    if (IsCanceled(callbacks)) return false;
    AString path;
    bool validPath;
    if (GetMemberPath(archive, i, defaultName, path, validPath) != S_OK ||
        !validPath || MemberIsSkippedNode(archive, i))
      continue;
    const int slash = path.Find('/');
    bool itemIsDirectory = false;
    if (GetMemberIsDirectory(archive, i, itemIsDirectory) != S_OK)
      unpackSizeDefined = false;
    if (!itemIsDirectory && unpackSizeDefined)
    {
      NWindows::NCOM::CPropVariant sizeProp;
      if (archive->GetProperty(i, kpidSize, &sizeProp) != S_OK ||
          sizeProp.vt != VT_UI8 ||
          sizeProp.uhVal.QuadPart > UINT64_MAX - unpackSize)
        unpackSizeDefined = false;
      else
        unpackSize += sizeProp.uhVal.QuadPart;
    }
    const bool isDirectory = slash >= 0 || itemIsDirectory;
    if (slash >= 0) path.DeleteFrom((unsigned)slash);
    struct stat st;
    if (!path.IsEmpty() &&
        fstatat(rootFd, path.Ptr(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        !S_ISLNK(st.st_mode) &&
        ((isDirectory && S_ISDIR(st.st_mode)) ||
         (!isDirectory && S_ISREG(st.st_mode))))
    {
      AddUniqueString(names, path);
    }
  }
  unsigned shown = 0;
  for (; shown < names.Size() && shown < 8; ++shown)
  {
    const unsigned extra = names[shown].Len() +
        (conflicts.IsEmpty() ? 0 : 2);
    if (conflicts.Len() + extra > 180) break;
    if (!conflicts.IsEmpty()) conflicts += ", ";
    conflicts += names[shown];
  }
  if (shown < names.Size())
    conflicts += conflicts.IsEmpty() ? "..." : ", ...";
  return !names.IsEmpty();
}

static HRESULT OpenWithRegisteredFormats(CCodecs &codecs,
    const UString &source, int preferredFormat,
    WfmOpenCallback *openSpec, CMyComPtr<IInArchive> &archive)
{
  const unsigned count = codecs.Formats.Size();
  for (unsigned pass = 0; pass <= count; ++pass)
  {
    const int formatIndex = pass == 0 ? preferredFormat : (int)(pass - 1);
    if (formatIndex < 0 || (unsigned)formatIndex >= count ||
        (pass != 0 && formatIndex == preferredFormat))
      continue;
    CMyComPtr<IInArchive> candidate;
    if (codecs.CreateInArchive((unsigned)formatIndex, candidate) != S_OK ||
        !candidate)
      continue;
    CInFileStream *fileSpec = new CInFileStream;
    CMyComPtr<IInStream> file = fileSpec;
    if (!fileSpec->Open(us2fs(source))) return E_FAIL;
    file = new WfmReadAheadStream(file);
    const UInt64 scanSize = 1 << 23;
    openSpec->PasswordRequested = false;
    openSpec->MissingVolume.Empty();
    const HRESULT result = candidate->Open(file, &scanSize, openSpec);
    if (result == S_OK)
    {
      archive = candidate;
      return S_OK;
    }
    candidate->Close();
    if (IsCanceled(openSpec->Callbacks)) return E_ABORT;
    if (openSpec->PasswordRequested || !openSpec->MissingVolume.IsEmpty())
      return result;
  }
  return S_FALSE;
}

int WfmEngineExtract(const char *source, const char *destination,
    const char *password, bool overwrite,
    const WfmEngineCallbacks *callbacks,
    char *message, unsigned int messageSize)
{
  WfmPerfStats perf(source);
  try
  {
    const bool splitContainer = EndsWithNoCase(source, ".001");
    if (splitContainer)
    {
      AString next(source);
      next.ReplaceOneCharAtPos(next.Len() - 1, '2');
      struct stat volumeStat;
      if (stat(next.Ptr(), &volumeStat) != 0)
      {
        const char *base = strrchr(next.Ptr(), '/');
        SetMessage(message, messageSize, base ? base + 1 : next.Ptr());
        return WFM_ENGINE_MISSING_VOLUME;
      }
    }
    UString sourceU;
    UString passwordU;
    if (!Utf8ToUnicode(source, sourceU) ||
        (password[0] && !Utf8ToUnicode(password, passwordU)))
    {
      SetMessage(message, messageSize, "Path or password is not valid UTF-8");
      return WFM_ENGINE_IO_ERROR;
    }
    CCodecs codecs;
    if (codecs.Load() != S_OK)
    {
      SetMessage(message, messageSize, "Cannot initialize archive codecs");
      return WFM_ENGINE_UNSUPPORTED_FORMAT;
    }
    const int preferredFormat = codecs.FindFormatForArchiveName(sourceU);
    CMyComPtr<IInArchive> archive;
    WfmOpenCallback *openSpec = new WfmOpenCallback(callbacks);
    CMyComPtr<IArchiveOpenCallback> openCallback = openSpec;
    openSpec->Password = passwordU;
    {
      const char *slash = strrchr(source, '/');
      AString name(slash ? slash + 1 : source);
      if (!ConvertUTF8ToUnicode(name, openSpec->ArchiveName))
        return WFM_ENGINE_IO_ERROR;
      if (slash)
        openSpec->SourceDirectory.SetFrom(source,
            (unsigned)(slash - source + 1));
      else
        openSpec->SourceDirectory = "./";
    }
    HRESULT result = OpenWithRegisteredFormats(codecs, sourceU,
        preferredFormat, openSpec, archive);
    if (result != S_OK)
    {
      if (IsCanceled(callbacks)) return WFM_ENGINE_CANCELED;
      if (!openSpec->MissingVolume.IsEmpty())
      {
        SetMessage(message, messageSize, openSpec->MissingVolume.Ptr());
        return WFM_ENGINE_MISSING_VOLUME;
      }
      if (openSpec->PasswordRequested)
      {
        if (!password[0])
        {
          SetMessage(message, messageSize, "Archive password is required");
          return WFM_ENGINE_PASSWORD_REQUIRED;
        }
        SetMessage(message, messageSize, "Archive password is incorrect");
        return WFM_ENGINE_BAD_PASSWORD;
      }
      SetMessage(message, messageSize, result == E_FAIL ?
          "Cannot open archive" : "Archive format is not available");
      return result == E_FAIL ? WFM_ENGINE_IO_ERROR :
          WFM_ENGINE_UNSUPPORTED_FORMAT;
    }

    bool unsafeRoot = false;
    WfmScopedFd rootFd(OpenRootDirectory(destination, &unsafeRoot));
    if (rootFd.Get() < 0)
    {
      if (unsafeRoot)
        SetMessage(message, messageSize,
            "Extraction directory contains a symbolic link");
      else
        SetMessage(message, messageSize, "Cannot create extraction directory");
      return unsafeRoot ? WFM_ENGINE_UNSAFE_PATH : WFM_ENGINE_IO_ERROR;
    }

    const bool compoundTar = IsCompoundTarSource(source);
    const bool recursiveContainer = compoundTar || splitContainer;
    WfmEngineCallbacks stageCallbacks;
    const WfmEngineCallbacks *extractCallbacks = callbacks;
    if (recursiveContainer && callbacks)
    {
      stageCallbacks = *callbacks;
      stageCallbacks.setProgress = NULL;
      stageCallbacks.setCurrentFile = NULL;
      extractCallbacks = &stageCallbacks;
    }
    struct stat sourceStat;
    const UInt64 packSize = stat(source, &sourceStat) == 0 &&
        sourceStat.st_size > 0 ? (UInt64)sourceStat.st_size : 0;
    WfmExtractCallback *extractSpec = new WfmExtractCallback(archive,
        source, rootFd.Get(), overwrite, packSize, extractCallbacks);
    CMyComPtr<IArchiveExtractCallback> extractCallback = extractSpec;
    extractSpec->Password = passwordU;
    AString conflicts;
    UInt64 unpackSize;
    bool unpackSizeDefined;
    const bool hasConflict = InspectArchiveItems(archive, rootFd.Get(),
        extractSpec->DefaultName(), callbacks, conflicts, unpackSize,
        unpackSizeDefined);
    if (IsCanceled(callbacks))
    {
      archive->Close();
      return WFM_ENGINE_CANCELED;
    }
    struct statvfs space;
    if (unpackSizeDefined && unpackSize &&
        fstatvfs(rootFd.Get(), &space) == 0 &&
        unpackSize > (UInt64)space.f_bavail * space.f_frsize)
    {
      archive->Close();
      SetMessage(message, messageSize, "Not enough free space for extraction");
      return WFM_ENGINE_NO_SPACE;
    }
    if (!overwrite && hasConflict)
    {
      archive->Close();
      SetMessage(message, messageSize, conflicts.Ptr());
      return WFM_ENGINE_OVERWRITE_REQUIRED;
    }
    perf.BeginExtract();
    result = archive->Extract(NULL, (UInt32)(Int32)-1, false, extractCallback);
    archive->Close();
    if (extractSpec->CurrentStreamNoSpace()) extractSpec->NoSpace = true;
    const bool canceled = IsCanceled(callbacks);
    const bool extractionFailed = canceled || extractSpec->UnsafePath ||
        extractSpec->OverwriteRequired || extractSpec->NoSpace ||
        extractSpec->IoError ||
        (extractSpec->PasswordRequested && !password[0]) || result != S_OK ||
        extractSpec->OperationResult != NArchive::NExtract::NOperationResult::kOK;
    if (extractionFailed) extractSpec->AbortCurrentOutput();
    if (canceled) return WFM_ENGINE_CANCELED;
    if (extractSpec->UnsafePath)
    {
      SetMessage(message, messageSize, "Archive contains an unsafe path");
      return WFM_ENGINE_UNSAFE_PATH;
    }
    if (extractSpec->OverwriteRequired)
    {
      SetMessage(message, messageSize,
          "Files or directories with the same name already exist");
      return WFM_ENGINE_OVERWRITE_REQUIRED;
    }
    if (extractSpec->NoSpace)
    {
      SetMessage(message, messageSize, "Not enough free space for extraction");
      return WFM_ENGINE_NO_SPACE;
    }
    if (extractSpec->IoError)
    {
      SetMessage(message, messageSize, "Cannot create extracted file");
      return WFM_ENGINE_IO_ERROR;
    }
    if (extractSpec->PasswordRequested && !password[0])
    {
      SetMessage(message, messageSize, "Archive password is required");
      return WFM_ENGINE_PASSWORD_REQUIRED;
    }
    if (result != S_OK || extractSpec->OperationResult !=
        NArchive::NExtract::NOperationResult::kOK)
    {
      SetMessage(message, messageSize, extractSpec->PasswordRequested ?
          "Archive password is incorrect or data is damaged" :
          "Archive data is damaged or unsupported");
      return extractSpec->PasswordRequested ? WFM_ENGINE_BAD_PASSWORD :
          WFM_ENGINE_DATA_ERROR;
    }
    if (compoundTar)
    {
      char nested[4096];
      if (snprintf(nested, sizeof(nested), "%s%s%s", destination,
          destination[strlen(destination) - 1] == '/' ? "" : "/",
          extractSpec->DefaultName().Ptr()) >= (int)sizeof(nested))
        return WFM_ENGINE_IO_ERROR;
      const int nestedResult = WfmEngineExtract(nested, destination, password,
          overwrite, callbacks, message, messageSize);
      if (nestedResult == WFM_ENGINE_OK)
      {
        unlink(nested);
        extractSpec->FinishOutputs();
      }
      else
      {
        unlink(nested);
        extractSpec->AbortCurrentOutput();
      }
      return nestedResult;
    }
    if (splitContainer)
    {
      const char *base = strrchr(source, '/');
      base = base ? base + 1 : source;
      AString nestedName(base);
      nestedName.DeleteFrom(nestedName.Len() - 4);
      char nested[4096];
      if (snprintf(nested, sizeof(nested), "%s%s%s", destination,
          destination[strlen(destination) - 1] == '/' ? "" : "/",
          nestedName.Ptr()) >= (int)sizeof(nested))
        return WFM_ENGINE_IO_ERROR;
      const int nestedResult = WfmEngineExtract(nested, destination, password,
          overwrite, callbacks, message, messageSize);
      if (nestedResult == WFM_ENGINE_OK)
      {
        unlink(nested);
        extractSpec->FinishOutputs();
        return WFM_ENGINE_OK;
      }
      if (nestedResult == WFM_ENGINE_UNSUPPORTED_FORMAT)
      {
        struct stat joinedStat;
        if (callbacks && callbacks->setProgress &&
            stat(nested, &joinedStat) == 0)
          callbacks->setProgress(callbacks->context,
              (UInt64)joinedStat.st_size, (UInt64)joinedStat.st_size);
        extractSpec->FinishOutputs();
        SetMessage(message, messageSize, "");
        return WFM_ENGINE_OK;
      }
      extractSpec->AbortCurrentOutput();
      return nestedResult;
    }
    extractSpec->FinishOutputs();
    return WFM_ENGINE_OK;
  }
  catch (...)
  {
    SetMessage(message, messageSize, "Archive extraction failed");
    return WFM_ENGINE_DATA_ERROR;
  }
}
