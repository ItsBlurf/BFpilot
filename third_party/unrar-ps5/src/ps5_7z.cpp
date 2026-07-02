#include "ps5_7z.hpp"

#include "../lzma2601/CPP/Common/MyInitGuid.h"
#include "../lzma2601/CPP/Common/MyCom.h"
#include "../lzma2601/CPP/Windows/PropVariant.h"
#include "../lzma2601/CPP/7zip/Archive/7z/7zHandler.h"
#include "../lzma2601/CPP/7zip/Archive/IArchive.h"
#include "../lzma2601/CPP/7zip/IPassword.h"
#include "../lzma2601/CPP/7zip/IStream.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" int posix_fallocate(int fd, off_t offset, off_t len);

extern "C" void Ps5UnrarProgress(int Percent);
extern "C" void Ps5ArchiveProgress64(unsigned long long Completed,
                                      unsigned long long Total);
extern "C" void Ps5ArchiveInputSample(unsigned long long Bytes,
                                      unsigned long long Usec);
extern "C" void Ps5ArchiveOutputSample(unsigned long long Bytes,
                                       unsigned long long Usec);

#define PS5_7Z_OUT_BUFFER_SIZE (8U * 1024U * 1024U)
#define PS5_7Z_PREALLOC_MIN_SIZE (64ULL * 1024ULL * 1024ULL)

static bool g_7z_output_error=false;

static unsigned long long Ps5ArchiveNowUsec7z()
{
  struct timespec Ts;
  if (clock_gettime(CLOCK_MONOTONIC,&Ts)!=0)
    return 0;
  return (unsigned long long)Ts.tv_sec*1000000ULL+
         (unsigned long long)(Ts.tv_nsec/1000ULL);
}

using namespace NArchive;
using namespace NWindows;

static std::string DirName7z(const std::string &Path)
{
  size_t Pos=Path.find_last_of('/');
  if (Pos==std::string::npos)
    return ".";
  if (Pos==0)
    return "/";
  return Path.substr(0,Pos);
}

static std::string BaseName7z(const std::string &Path)
{
  size_t Pos=Path.find_last_of('/');
  return Pos==std::string::npos ? Path:Path.substr(Pos+1);
}

static std::string ToLowerAscii7z(const std::string &Value)
{
  std::string Lower=Value;
  for (size_t I=0;I<Lower.size();I++)
    Lower[I]=(char)std::tolower((unsigned char)Lower[I]);
  return Lower;
}

static std::string JoinPath7z(const std::string &Left,const std::string &Right)
{
  if (Left.empty() || Right.empty() || Right[0]=='/')
    return Left.empty() ? Right:Left;
  if (Left[Left.size()-1]=='/')
    return Left+Right;
  return Left+"/"+Right;
}

static bool MkdirAll7z(const std::string &Path)
{
  if (Path.empty() || Path=="/")
    return true;

  std::string Cur;
  size_t Pos=Path[0]=='/' ? 1:0;
  if (Path[0]=='/')
    Cur="/";

  while (Pos<=Path.size())
  {
    size_t Next=Path.find('/',Pos);
    std::string Part=Path.substr(Pos,Next==std::string::npos ? std::string::npos:Next-Pos);
    if (!Part.empty())
    {
      if (!Cur.empty() && Cur[Cur.size()-1]!='/')
        Cur+="/";
      Cur+=Part;
      if (mkdir(Cur.c_str(),0777)!=0 && errno!=EEXIST)
        return false;
    }
    if (Next==std::string::npos)
      break;
    Pos=Next+1;
  }
  return true;
}

static UString AsciiToUString(const std::string &Value)
{
  UString Out;
  for (size_t I=0;I<Value.size();I++)
    Out+=(wchar_t)(unsigned char)Value[I];
  return Out;
}

static std::string WideToUtf8(const wchar_t *Value,unsigned Len)
{
  std::string Out;
  for (unsigned I=0;I<Len;I++)
  {
    unsigned C=(unsigned)Value[I];
    if (C<0x80)
      Out+=(char)C;
    else if (C<0x800)
    {
      Out+=(char)(0xc0 | (C>>6));
      Out+=(char)(0x80 | (C&0x3f));
    }
    else if (C<0x10000)
    {
      Out+=(char)(0xe0 | (C>>12));
      Out+=(char)(0x80 | ((C>>6)&0x3f));
      Out+=(char)(0x80 | (C&0x3f));
    }
    else
    {
      Out+=(char)(0xf0 | (C>>18));
      Out+=(char)(0x80 | ((C>>12)&0x3f));
      Out+=(char)(0x80 | ((C>>6)&0x3f));
      Out+=(char)(0x80 | (C&0x3f));
    }
  }
  return Out;
}

static std::string BstrToUtf8(BSTR Value)
{
  if (!Value)
    return std::string();
  return WideToUtf8(Value,::SysStringLen(Value));
}

static bool IsSafeMemberPath(const std::string &Path)
{
  if (Path.empty() || Path[0]=='/' || Path[0]=='\\')
    return false;
  if (Path.size()>=2 && std::isalpha((unsigned char)Path[0]) && Path[1]==':')
    return false;

  size_t Pos=0;
  while (Pos<=Path.size())
  {
    size_t Next=Path.find_first_of("/\\",Pos);
    std::string Part=Path.substr(Pos,Next==std::string::npos ? std::string::npos:Next-Pos);
    if (Part==".." || Part.empty())
      return false;
    if (Next==std::string::npos)
      break;
    Pos=Next+1;
  }
  return true;
}

static std::string NormalizeMemberPath(const std::string &Path)
{
  std::string Out=Path;
  for (size_t I=0;I<Out.size();I++)
    if (Out[I]=='\\')
      Out[I]='/';
  return Out;
}

static HRESULT ErrnoToHresult()
{
  if (errno==ENOMEM)
    return E_OUTOFMEMORY;
  return E_FAIL;
}

static bool Preallocate7zOutput(int Fd,UInt64 ExpectedSize)
{
  if (ExpectedSize<PS5_7Z_PREALLOC_MIN_SIZE ||
      ExpectedSize>(UInt64)LLONG_MAX)
    return true;

  int Rc=posix_fallocate(Fd,0,(off_t)ExpectedSize);
  if (Rc==0)
    return true;
  if (Rc==ENOSPC || Rc==EFBIG)
  {
    errno=Rc;
    return false;
  }
  return true;
}

static bool PropVariantToUInt64(const NCOM::CPropVariant &Prop,UInt64 *Out);

class CPosixInStream Z7_final:
  public IInStream,
  public IStreamGetSize,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IInStream,ISequentialInStream,IStreamGetSize)
  Z7_IFACE_COM7_IMP(ISequentialInStream)
  Z7_IFACE_COM7_IMP(IInStream)
  Z7_IFACE_COM7_IMP(IStreamGetSize)

  int Fd;
  UInt64 Size;

public:
  CPosixInStream():Fd(-1),Size(0) {}
  ~CPosixInStream()
  {
    if (Fd>=0)
      close(Fd);
  }

  bool Open(const std::string &Path)
  {
    Fd=open(Path.c_str(),O_RDONLY);
    if (Fd<0)
      return false;
    struct stat St;
    if (fstat(Fd,&St)!=0)
    {
      int SavedErrno=errno;
      close(Fd);
      Fd=-1;
      errno=SavedErrno;
      return false;
    }
    if (!S_ISREG(St.st_mode))
    {
      close(Fd);
      Fd=-1;
      errno=EINVAL;
      return false;
    }
    Size=(UInt64)St.st_size;
    return true;
  }
};

Z7_COM7F_IMF(CPosixInStream::Read(void *Data,UInt32 SizeToRead,UInt32 *ProcessedSize))
{
  if (ProcessedSize)
    *ProcessedSize=0;
  if (SizeToRead==0)
    return S_OK;
  unsigned long long Start=Ps5ArchiveNowUsec7z();
  ssize_t ReadSize;
  do
  {
    ReadSize=read(Fd,Data,SizeToRead);
  } while (ReadSize<0 && errno==EINTR);
  if (ReadSize<0)
    return ErrnoToHresult();
  if (ReadSize>0)
  {
    unsigned long long End=Ps5ArchiveNowUsec7z();
    Ps5ArchiveInputSample((unsigned long long)ReadSize,
                          End>Start ? End-Start:0);
  }
  if (ProcessedSize)
    *ProcessedSize=(UInt32)ReadSize;
  return S_OK;
}

Z7_COM7F_IMF(CPosixInStream::Seek(Int64 Offset,UInt32 SeekOrigin,UInt64 *NewPosition))
{
  int Whence=SEEK_SET;
  if (SeekOrigin==STREAM_SEEK_CUR)
    Whence=SEEK_CUR;
  else if (SeekOrigin==STREAM_SEEK_END)
    Whence=SEEK_END;
  else if (SeekOrigin!=STREAM_SEEK_SET)
    return STG_E_INVALIDFUNCTION;

  off_t Pos=lseek(Fd,(off_t)Offset,Whence);
  if (Pos<0)
    return ErrnoToHresult();
  if (NewPosition)
    *NewPosition=(UInt64)Pos;
  return S_OK;
}

Z7_COM7F_IMF(CPosixInStream::GetSize(UInt64 *OutSize))
{
  *OutSize=Size;
  return S_OK;
}

struct CVolumeInfo
{
  std::string path;
  UInt64 size;
  UInt64 start;
};

class CVolumeInStream Z7_final:
  public IInStream,
  public IStreamGetSize,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IInStream,ISequentialInStream,IStreamGetSize)
  Z7_IFACE_COM7_IMP(ISequentialInStream)
  Z7_IFACE_COM7_IMP(IInStream)
  Z7_IFACE_COM7_IMP(IStreamGetSize)

  std::vector<CVolumeInfo> Volumes;
  UInt64 Pos;
  UInt64 TotalSize;
  int CurrentFd;
  size_t CurrentIndex;

  bool OpenVolume(size_t Index)
  {
    if (CurrentIndex==Index && CurrentFd>=0)
      return true;
    if (CurrentFd>=0)
      close(CurrentFd);
    CurrentFd=open(Volumes[Index].path.c_str(),O_RDONLY);
    CurrentIndex=Index;
    return CurrentFd>=0;
  }

public:
  std::string MissingVolume;

  CVolumeInStream():Pos(0),TotalSize(0),CurrentFd(-1),CurrentIndex((size_t)-1) {}
  ~CVolumeInStream()
  {
    if (CurrentFd>=0)
      close(CurrentFd);
  }

  bool Open(const std::string &ArchivePath)
  {
    std::string Name=BaseName7z(ArchivePath);
    if (Name.size()<3)
      return false;
    std::string Prefix=JoinPath7z(DirName7z(ArchivePath),Name.substr(0,Name.size()-3));

    for (unsigned Part=1;Part<=999;Part++)
    {
      char Suffix[4];
      snprintf(Suffix,sizeof(Suffix),"%03u",Part);
      std::string Path=Prefix+Suffix;
      struct stat St;
      if (stat(Path.c_str(),&St)!=0)
      {
        if (Part==1)
          return false;
        break;
      }
      if (!S_ISREG(St.st_mode))
        break;
      CVolumeInfo Info;
      Info.path=Path;
      Info.size=(UInt64)St.st_size;
      Info.start=TotalSize;
      TotalSize+=Info.size;
      Volumes.push_back(Info);
    }
    if (!Volumes.empty())
    {
      for (unsigned Part=(unsigned)Volumes.size()+2;Part<=999;Part++)
      {
        char Suffix[4];
        snprintf(Suffix,sizeof(Suffix),"%03u",Part);
        std::string Path=Prefix+Suffix;
        struct stat St;
        if (stat(Path.c_str(),&St)==0)
        {
          char MissingSuffix[4];
          snprintf(MissingSuffix,sizeof(MissingSuffix),"%03u",(unsigned)Volumes.size()+1);
          MissingVolume=Prefix+MissingSuffix;
          break;
        }
      }
    }
    return !Volumes.empty();
  }

  const std::vector<CVolumeInfo> &GetVolumes() const { return Volumes; }
};

Z7_COM7F_IMF(CVolumeInStream::Read(void *Data,UInt32 SizeToRead,UInt32 *ProcessedSize))
{
  if (ProcessedSize)
    *ProcessedSize=0;
  if (SizeToRead==0)
    return S_OK;
  if (Pos>=TotalSize)
  {
    return S_OK;
  }

  UInt32 Done=0;
  while (Done<SizeToRead && Pos<TotalSize)
  {
    size_t Index=0;
    while (Index+1<Volumes.size() && Pos>=Volumes[Index].start+Volumes[Index].size)
      Index++;
    if (!OpenVolume(Index))
      return ErrnoToHresult();
    UInt64 LocalPos=Pos-Volumes[Index].start;
    if (lseek(CurrentFd,(off_t)LocalPos,SEEK_SET)<0)
      return ErrnoToHresult();
    UInt64 Avail=Volumes[Index].size-LocalPos;
    UInt32 Cur=(UInt32)std::min<UInt64>(Avail,SizeToRead-Done);
    unsigned long long Start=Ps5ArchiveNowUsec7z();
    ssize_t ReadSize;
    do
    {
      ReadSize=read(CurrentFd,(Byte *)Data+Done,Cur);
    } while (ReadSize<0 && errno==EINTR);
    if (ReadSize<0)
      return ErrnoToHresult();
    if (ReadSize==0)
      break;
    unsigned long long End=Ps5ArchiveNowUsec7z();
    Ps5ArchiveInputSample((unsigned long long)ReadSize,
                          End>Start ? End-Start:0);
    Done+=(UInt32)ReadSize;
    Pos+=(UInt32)ReadSize;
  }
  if (ProcessedSize)
    *ProcessedSize=Done;
  return S_OK;
}

Z7_COM7F_IMF(CVolumeInStream::Seek(Int64 Offset,UInt32 SeekOrigin,UInt64 *NewPosition))
{
  UInt64 Base=0;
  if (SeekOrigin==STREAM_SEEK_SET)
    Base=0;
  else if (SeekOrigin==STREAM_SEEK_CUR)
    Base=Pos;
  else if (SeekOrigin==STREAM_SEEK_END)
    Base=TotalSize;
  else
    return STG_E_INVALIDFUNCTION;

  if (Offset<0 && Base<(UInt64)(-Offset))
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  Pos=Offset<0 ? Base-(UInt64)(-Offset):Base+(UInt64)Offset;
  if (NewPosition)
    *NewPosition=Pos;
  return S_OK;
}

Z7_COM7F_IMF(CVolumeInStream::GetSize(UInt64 *OutSize))
{
  *OutSize=TotalSize;
  return S_OK;
}

class CPosixOutStream Z7_final:
  public ISequentialOutStream,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
  Z7_IFACE_COM7_IMP(ISequentialOutStream)

  int Fd;
  std::vector<Byte> Buffer;
  size_t Used;
  bool Failed;

  HRESULT FlushBuffer()
  {
    if (Failed)
      return E_FAIL;
    size_t Done=0;
    while (Done<Used)
    {
      unsigned long long Start=Ps5ArchiveNowUsec7z();
      ssize_t Written=write(Fd,Buffer.data()+Done,Used-Done);
      if (Written<0)
      {
        if (errno==EINTR)
          continue;
        Failed=true;
        g_7z_output_error=true;
        return ErrnoToHresult();
      }
      if (Written==0)
      {
        errno=ENOSPC;
        Failed=true;
        g_7z_output_error=true;
        return ErrnoToHresult();
      }
      unsigned long long End=Ps5ArchiveNowUsec7z();
      Ps5ArchiveOutputSample((unsigned long long)Written,
                             End>Start ? End-Start:0);
      Done+=(size_t)Written;
    }
    Used=0;
    return S_OK;
  }

public:
  CPosixOutStream():Fd(-1),Used(0),Failed(false) {}
  ~CPosixOutStream()
  {
    if (Fd>=0 && Used>0)
      FlushBuffer();
    if (Fd>=0)
      close(Fd);
  }

  bool Create(const std::string &Path,UInt64 ExpectedSize)
  {
    if (Fd>=0)
    {
      if (Used>0)
        FlushBuffer();
      close(Fd);
      Fd=-1;
    }
    Used=0;
    Failed=false;
    if (Buffer.empty())
    {
      try
      {
        Buffer.resize(PS5_7Z_OUT_BUFFER_SIZE);
      }
      catch(...)
      {
        errno=ENOMEM;
        return false;
      }
    }
    Fd=open(Path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);
    if (Fd>=0 && ExpectedSize>0 && ExpectedSize<=(UInt64)LLONG_MAX)
    {
      if (!Preallocate7zOutput(Fd,ExpectedSize))
      {
        int SavedErrno=errno;
        close(Fd);
        Fd=-1;
        errno=SavedErrno;
        return false;
      }
      (void)ftruncate(Fd,(off_t)ExpectedSize);
    }
    return Fd>=0;
  }
};

Z7_COM7F_IMF(CPosixOutStream::Write(const void *Data,UInt32 SizeToWrite,UInt32 *ProcessedSize))
{
  if (ProcessedSize)
    *ProcessedSize=0;
  if (SizeToWrite==0)
    return S_OK;
  if (Failed)
    return E_FAIL;
  UInt32 Done=0;
  while (Done<SizeToWrite)
  {
    size_t Space=Buffer.size()-Used;
    if (Space==0)
    {
      HRESULT Res=FlushBuffer();
      if (Res!=S_OK)
        return Res;
      Space=Buffer.size();
    }
    UInt32 Cur=(UInt32)std::min<size_t>(Space,SizeToWrite-Done);
    memcpy(Buffer.data()+Used,(const Byte *)Data+Done,Cur);
    Used+=Cur;
    Done+=Cur;
  }
  if (ProcessedSize)
    *ProcessedSize=Done;
  return S_OK;
}

class COpenCallback Z7_final:
  public IArchiveOpenCallback,
  public IArchiveOpenVolumeCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IArchiveOpenCallback,IArchiveOpenVolumeCallback,ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IArchiveOpenCallback)
  Z7_IFACE_COM7_IMP(IArchiveOpenVolumeCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)

  std::string Folder;
  std::string Name;
  UString Password;
  bool PasswordDefined;

public:
  COpenCallback(const std::string &ArchivePath,const std::string &PasswordUtf8):
    Folder(DirName7z(ArchivePath)),Name(BaseName7z(ArchivePath)),
    Password(AsciiToUString(PasswordUtf8)),PasswordDefined(!PasswordUtf8.empty()) {}

  std::string MissingVolume;
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *,const UInt64 *))
{
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *,const UInt64 *))
{
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::GetProperty(PROPID PropID,PROPVARIANT *Value))
{
  NCOM::CPropVariant Prop;
  if (PropID==kpidName)
    Prop=AsciiToUString(Name);
  Prop.Detach(Value);
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::GetStream(const wchar_t *NameW,IInStream **InStream))
{
  *InStream=NULL;
  std::string Requested=WideToUtf8(NameW,(unsigned)wcslen(NameW));
  Requested=BaseName7z(Requested);
  if (!IsSafeMemberPath(Requested))
    return S_FALSE;

  std::string Path=JoinPath7z(Folder,Requested);
  CPosixInStream *StreamSpec=new CPosixInStream;
  CMyComPtr<IInStream> Stream=StreamSpec;
  if (!StreamSpec->Open(Path))
  {
    MissingVolume=Path;
    return S_FALSE;
  }
  *InStream=Stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *OutPassword))
{
  if (!PasswordDefined)
    return E_ABORT;
  return StringToBstr(Password,OutPassword);
}

class CExtractCallback Z7_final:
  public IArchiveExtractCallback,
  public IArchiveExtractCallbackMessage2,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IArchiveExtractCallback,IArchiveExtractCallbackMessage2,ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallbackMessage2)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)

  CMyComPtr<IInArchive> Archive;
  std::string DestPath;
  UString Password;
  bool PasswordDefined;
  UInt64 Total;
  int LastOpResult;

public:
  CExtractCallback(IInArchive *ArchiveHandler,const std::string &Dest,
                   const std::string &PasswordUtf8):
    Archive(ArchiveHandler),DestPath(Dest),Password(AsciiToUString(PasswordUtf8)),
    PasswordDefined(!PasswordUtf8.empty()),Total(0),LastOpResult(NExtract::NOperationResult::kOK) {}

  int GetLastOpResult() const { return LastOpResult; }
};

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64 TotalValue))
{
  Total=TotalValue;
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64 *CompleteValue))
{
  if (CompleteValue && Total>0)
  {
    UInt64 Percent=(*CompleteValue)*100/Total;
    Ps5ArchiveProgress64((unsigned long long)*CompleteValue,
                         (unsigned long long)Total);
    Ps5UnrarProgress((int)std::min<UInt64>(Percent,100));
  }
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::GetStream(UInt32 Index,ISequentialOutStream **OutStream,
                                         Int32 AskExtractMode))
{
  *OutStream=NULL;
  if (AskExtractMode!=NExtract::NAskMode::kExtract)
    return S_OK;

  NCOM::CPropVariant Prop;
  RINOK(Archive->GetProperty(Index,kpidPath,&Prop))

  std::string Member;
  if (Prop.vt==VT_EMPTY)
    Member="empty";
  else if (Prop.vt==VT_BSTR)
    Member=BstrToUtf8(Prop.bstrVal);
  else
    return E_FAIL;

  Member=NormalizeMemberPath(Member);
  if (!IsSafeMemberPath(Member))
    return E_FAIL;

  bool IsDir=false;
  NCOM::CPropVariant IsDirProp;
  RINOK(Archive->GetProperty(Index,kpidIsDir,&IsDirProp))
  if (IsDirProp.vt==VT_BOOL)
    IsDir=IsDirProp.boolVal!=VARIANT_FALSE;

  std::string FullPath=JoinPath7z(DestPath,Member);
  if (IsDir)
  {
    return MkdirAll7z(FullPath) ? S_OK:E_FAIL;
  }

  UInt64 ExpectedSize=0;
  NCOM::CPropVariant SizeProp;
  if (Archive->GetProperty(Index,kpidSize,&SizeProp)==S_OK)
    (void)PropVariantToUInt64(SizeProp,&ExpectedSize);

  std::string Parent=DirName7z(FullPath);
  if (!MkdirAll7z(Parent))
    return E_FAIL;

  CPosixOutStream *StreamSpec=new CPosixOutStream;
  CMyComPtr<ISequentialOutStream> Stream=StreamSpec;
  if (!StreamSpec->Create(FullPath,ExpectedSize))
    return ErrnoToHresult();

  *OutStream=Stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::PrepareOperation(Int32))
{
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32 OpRes))
{
  if (OpRes!=NExtract::NOperationResult::kOK)
    LastOpResult=OpRes;
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::ReportExtractResult(UInt32,UInt32,Int32 OpRes))
{
  if (OpRes!=NExtract::NOperationResult::kOK)
    LastOpResult=OpRes;
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR *OutPassword))
{
  if (!PasswordDefined)
    return E_ABORT;
  return StringToBstr(Password,OutPassword);
}

static const char *OperationResultReason(int OpRes)
{
  switch(OpRes)
  {
    case NExtract::NOperationResult::kUnsupportedMethod: return "unsupported 7z method";
    case NExtract::NOperationResult::kDataError: return "7z data or password error";
    case NExtract::NOperationResult::kCRCError: return "checksum or password error";
    case NExtract::NOperationResult::kUnavailable: return "missing 7z volume or unavailable data";
    case NExtract::NOperationResult::kUnexpectedEnd: return "missing 7z volume or unexpected end";
    case NExtract::NOperationResult::kDataAfterEnd: return "7z data after end";
    case NExtract::NOperationResult::kIsNotArc: return "not a 7z archive";
    case NExtract::NOperationResult::kHeadersError: return "7z headers error";
    case NExtract::NOperationResult::kWrongPassword: return "bad password";
  }
  return "7z extraction error";
}

static RAR_EXIT OperationResultCode(int OpRes)
{
  switch(OpRes)
  {
    case NExtract::NOperationResult::kUnsupportedMethod: return RARX_FATAL;
    case NExtract::NOperationResult::kCRCError:
    case NExtract::NOperationResult::kWrongPassword:
    case NExtract::NOperationResult::kDataError: return RARX_CRC;
    case NExtract::NOperationResult::kUnavailable:
    case NExtract::NOperationResult::kUnexpectedEnd: return RARX_OPEN;
    default: return RARX_FATAL;
  }
}

static void Set7zProperties(IInArchive *Archive,uint Threads)
{
  CMyComPtr<ISetProperties> SetProperties;
  Archive->QueryInterface(IID_ISetProperties,(void **)&SetProperties);
  if (!SetProperties)
    return;

  const wchar_t *Names[1]={ L"mt" };
  NCOM::CPropVariant Values[1];
  Values[0]=(UInt32)std::min<uint>(Threads>0 ? Threads:1,8);
  SetProperties->SetProperties(Names,Values,1);
}

static bool PropVariantToUInt64(const NCOM::CPropVariant &Prop,UInt64 *Out)
{
  switch (Prop.vt)
  {
    case VT_UI8: *Out=Prop.uhVal.QuadPart; return true;
    case VT_UI4: *Out=Prop.ulVal; return true;
    case VT_UI2: *Out=Prop.uiVal; return true;
    case VT_UI1: *Out=Prop.bVal; return true;
    case VT_I8:
      if (Prop.hVal.QuadPart<0)
        return false;
      *Out=(UInt64)Prop.hVal.QuadPart;
      return true;
    case VT_I4:
      if (Prop.lVal<0)
        return false;
      *Out=(UInt64)Prop.lVal;
      return true;
  }
  return false;
}

SevenZArchiveInfo Probe7zArchive(const std::string &ArchivePath,
                                 const std::string &Password,
                                 uint Threads)
{
  SevenZArchiveInfo Info;
  Info.code=RARX_FATAL;
  Info.reason="7z probe error";
  Info.input_bytes=0;
  Info.total_bytes=0;
  Info.max_file_bytes=0;
  Info.file_count=0;
  Info.dir_count=0;
  Info.unknown_size_count=0;

  CMyComPtr<IInArchive> Archive=new NArchive::N7z::CHandler;
  Set7zProperties(Archive,Threads);

  CMyComPtr<IInStream> InStream;
  CVolumeInStream *VolumeStreamSpec=NULL;
  std::string LowerArchivePath=ToLowerAscii7z(ArchivePath);
  if (LowerArchivePath.size()>7 && LowerArchivePath.substr(LowerArchivePath.size()-7)==".7z.001")
  {
    VolumeStreamSpec=new CVolumeInStream;
    CMyComPtr<IInStream> Temp=VolumeStreamSpec;
    if (!VolumeStreamSpec->Open(ArchivePath))
    {
      Info.code=RARX_OPEN;
      Info.reason="missing 7z volume or open error";
      Info.missing_volume=ArchivePath;
      return Info;
    }
    if (!VolumeStreamSpec->MissingVolume.empty())
    {
      Info.code=RARX_OPEN;
      Info.reason="missing 7z volume or open error";
      Info.missing_volume=VolumeStreamSpec->MissingVolume;
      return Info;
    }
    InStream=Temp;
  }
  else
  {
    CPosixInStream *FileStreamSpec=new CPosixInStream;
    CMyComPtr<IInStream> Temp=FileStreamSpec;
    if (!FileStreamSpec->Open(ArchivePath))
    {
      Info.code=RARX_OPEN;
      Info.reason="open error";
      return Info;
    }
    InStream=Temp;
  }

  CMyComPtr<IStreamGetSize> SizeGetter;
  InStream->QueryInterface(IID_IStreamGetSize,(void **)&SizeGetter);
  if (SizeGetter)
  {
    UInt64 Size=0;
    if (SizeGetter->GetSize(&Size)==S_OK)
      Info.input_bytes=(unsigned long long)Size;
  }

  COpenCallback *OpenCallbackSpec=new COpenCallback(ArchivePath,Password);
  CMyComPtr<IArchiveOpenCallback> OpenCallback=OpenCallbackSpec;
  UInt64 ScanSize=(UInt64)1<<23;
  HRESULT OpenResult=Archive->Open(InStream,&ScanSize,OpenCallback);
  if (OpenResult!=S_OK)
  {
    if (OpenResult==E_OUTOFMEMORY)
    {
      Info.code=RARX_MEMORY;
      Info.reason="out of memory";
    }
    else if (OpenResult==E_ABORT && Password.empty())
    {
      Info.code=RARX_BADPWD;
      Info.reason="7z password required";
    }
    else if (OpenResult==E_ABORT)
    {
      Info.code=RARX_BADPWD;
      Info.reason="bad 7z password or probe aborted";
    }
    else if (Password.empty())
    {
      Info.code=RARX_OPEN;
      Info.reason="missing 7z volume or open error";
    }
    else
    {
      Info.code=RARX_BADPWD;
      Info.reason="bad 7z password or encrypted-header open error";
    }
    if (VolumeStreamSpec && !VolumeStreamSpec->MissingVolume.empty())
      Info.missing_volume=VolumeStreamSpec->MissingVolume;
    else if (!OpenCallbackSpec->MissingVolume.empty())
      Info.missing_volume=OpenCallbackSpec->MissingVolume;
    return Info;
  }

  UInt32 Count=0;
  if (Archive->GetNumberOfItems(&Count)!=S_OK)
  {
    Info.code=RARX_FATAL;
    Info.reason="7z item count probe failed";
    return Info;
  }

  for (UInt32 I=0;I<Count;I++)
  {
    bool IsDir=false;
    NCOM::CPropVariant IsDirProp;
    if (Archive->GetProperty(I,kpidIsDir,&IsDirProp)==S_OK &&
        IsDirProp.vt==VT_BOOL)
      IsDir=IsDirProp.boolVal!=VARIANT_FALSE;

    if (IsDir)
    {
      Info.dir_count++;
      continue;
    }

    Info.file_count++;
    NCOM::CPropVariant SizeProp;
    UInt64 Size=0;
    if (Archive->GetProperty(I,kpidSize,&SizeProp)==S_OK &&
        PropVariantToUInt64(SizeProp,&Size))
    {
      if (Info.total_bytes>ULLONG_MAX-(unsigned long long)Size)
        Info.total_bytes=ULLONG_MAX;
      else
        Info.total_bytes+=(unsigned long long)Size;
      if ((unsigned long long)Size>Info.max_file_bytes)
        Info.max_file_bytes=(unsigned long long)Size;
    }
    else
    {
      Info.unknown_size_count++;
    }
  }

  Info.code=RARX_SUCCESS;
  Info.reason="success";
  return Info;
}

SevenZExtractResult Run7zExtract(const std::string &ArchivePath,
                                 const std::string &DestPath,
                                 const std::string &Password,
                                 uint Threads)
{
  SevenZExtractResult Result;
  Result.code=RARX_FATAL;
  Result.reason="7z extraction error";

  CMyComPtr<IInArchive> Archive=new NArchive::N7z::CHandler;
  Set7zProperties(Archive,Threads);

  CMyComPtr<IInStream> InStream;
  CVolumeInStream *VolumeStreamSpec=NULL;
  std::string LowerArchivePath=ToLowerAscii7z(ArchivePath);
  if (LowerArchivePath.size()>7 && LowerArchivePath.substr(LowerArchivePath.size()-7)==".7z.001")
  {
    VolumeStreamSpec=new CVolumeInStream;
    CMyComPtr<IInStream> Temp=VolumeStreamSpec;
    if (!VolumeStreamSpec->Open(ArchivePath))
    {
      Result.code=RARX_OPEN;
      Result.reason="missing 7z volume or open error";
      Result.missing_volume=ArchivePath;
      return Result;
    }
    if (!VolumeStreamSpec->MissingVolume.empty())
    {
      Result.code=RARX_OPEN;
      Result.reason="missing 7z volume or open error";
      Result.missing_volume=VolumeStreamSpec->MissingVolume;
      return Result;
    }
    InStream=Temp;
  }
  else
  {
    CPosixInStream *FileStreamSpec=new CPosixInStream;
    CMyComPtr<IInStream> Temp=FileStreamSpec;
    if (!FileStreamSpec->Open(ArchivePath))
    {
      Result.code=RARX_OPEN;
      Result.reason="open error";
      return Result;
    }
    InStream=Temp;
  }

  COpenCallback *OpenCallbackSpec=new COpenCallback(ArchivePath,Password);
  CMyComPtr<IArchiveOpenCallback> OpenCallback=OpenCallbackSpec;
  UInt64 ScanSize=(UInt64)1<<23;
  HRESULT OpenResult=Archive->Open(InStream,&ScanSize,OpenCallback);
  if (OpenResult!=S_OK)
  {
    if (OpenResult==E_OUTOFMEMORY)
    {
      Result.code=RARX_MEMORY;
      Result.reason="out of memory";
    }
    else if (OpenResult==E_ABORT && Password.empty())
    {
      Result.code=RARX_BADPWD;
      Result.reason="7z password required";
    }
    else if (OpenResult==E_ABORT)
    {
      Result.code=RARX_BADPWD;
      Result.reason="bad 7z password or extraction aborted";
    }
    else if (Password.empty())
    {
      Result.code=RARX_OPEN;
      Result.reason="missing 7z volume or open error";
    }
    else
    {
      Result.code=RARX_BADPWD;
      Result.reason="bad 7z password or encrypted-header open error";
    }
    if (VolumeStreamSpec && !VolumeStreamSpec->MissingVolume.empty())
      Result.missing_volume=VolumeStreamSpec->MissingVolume;
    else if (!OpenCallbackSpec->MissingVolume.empty())
      Result.missing_volume=OpenCallbackSpec->MissingVolume;
    return Result;
  }

  g_7z_output_error=false;
  CExtractCallback *ExtractCallbackSpec=new CExtractCallback(Archive,DestPath,Password);
  CMyComPtr<IArchiveExtractCallback> ExtractCallback=ExtractCallbackSpec;
  HRESULT ExtractResult=Archive->Extract(NULL,(UInt32)(Int32)-1,false,ExtractCallback);
  if (ExtractResult!=S_OK)
  {
    if (ExtractResult==E_OUTOFMEMORY)
    {
      Result.code=RARX_MEMORY;
      Result.reason="out of memory";
    }
    else if (ExtractResult==E_ABORT && Password.empty())
    {
      Result.code=RARX_BADPWD;
      Result.reason="7z password required";
    }
    else if (ExtractResult==E_ABORT)
    {
      Result.code=RARX_BADPWD;
      Result.reason="bad 7z password or extraction aborted";
    }
    else
    {
      Result.code=RARX_FATAL;
      Result.reason="7z extraction error";
    }
    if (VolumeStreamSpec && !VolumeStreamSpec->MissingVolume.empty())
    {
      Result.code=RARX_OPEN;
      Result.reason="missing 7z volume or open error";
      Result.missing_volume=VolumeStreamSpec->MissingVolume;
    }
    return Result;
  }
  if (g_7z_output_error)
  {
    Result.code=RARX_WRITE;
    Result.reason="7z output write error";
    return Result;
  }

  int OpRes=ExtractCallbackSpec->GetLastOpResult();
  if (OpRes!=NExtract::NOperationResult::kOK)
  {
    Result.code=OperationResultCode(OpRes);
    Result.reason=OperationResultReason(OpRes);
    if (VolumeStreamSpec && !VolumeStreamSpec->MissingVolume.empty())
      Result.missing_volume=VolumeStreamSpec->MissingVolume;
    return Result;
  }

  Result.code=RARX_SUCCESS;
  Result.reason="success";
  return Result;
}
