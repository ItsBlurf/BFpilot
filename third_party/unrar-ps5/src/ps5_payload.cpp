#include "rar.hpp"
#include "ps5_7z.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/cpuset.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define UNRAR_DATA_DIR "/data/unrar"
#define UNRAR_CONFIG UNRAR_DATA_DIR "/config.ini"
#define DEFAULT_EXTRACT_LOCATION "/data/homebrew"
#define STAGING_DIR_NAME ".unrar-staging"

typedef struct notify_request
{
  char unused[45];
  char message[3075];
} notify_request_t;

extern "C" int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

struct PayloadConfig
{
  std::vector<std::string> filenames;
  std::string config_path;
  std::string archive_location;
  std::string archive_password;
  bool delete_after;
  std::string extract_location;
  uint threads;
  int nice;
  uint64 cpu_mask;
  int progress;
};

struct LockGuard
{
  int fd;
  std::string path;

  LockGuard():fd(-1),path(UNRAR_DATA_DIR "/unrar.lock") {}
  ~LockGuard()
  {
    if (fd>=0)
      close(fd);
  }

  bool Acquire()
  {
    fd=open(path.c_str(),O_WRONLY|O_CREAT,0666);
    if (fd<0)
      return false;

    struct flock Lock;
    memset(&Lock,0,sizeof(Lock));
    Lock.l_type=F_WRLCK;
    Lock.l_whence=SEEK_SET;
    if (fcntl(fd,F_SETLK,&Lock)!=0)
    {
      close(fd);
      fd=-1;
      return false;
    }

    ftruncate(fd,0);
    char Pid[32];
    int Len=snprintf(Pid,sizeof(Pid),"%d\n",(int)getpid());
    if (Len>0)
      write(fd,Pid,(size_t)Len);
    return true;
  }
};

static int LastNotifiedProgress=-10;
static int ProgressInterval=10;
static cpuset_t OriginalCpuAffinity;
static bool HaveOriginalCpuAffinity=false;
static bool CpuAffinityApplied=false;

static bool EndsWithNoCase(const std::string &Value,const std::string &Suffix)
{
  if (Value.size()<Suffix.size())
    return false;
  size_t Offset=Value.size()-Suffix.size();
  for (size_t I=0;I<Suffix.size();I++)
    if (std::tolower((unsigned char)Value[Offset+I])!=std::tolower((unsigned char)Suffix[I]))
      return false;
  return true;
}

static std::string Trim(const std::string &Value)
{
  size_t Start=0;
  while (Start<Value.size() && std::isspace((unsigned char)Value[Start]))
    Start++;

  size_t End=Value.size();
  while (End>Start && std::isspace((unsigned char)Value[End-1]))
    End--;

  return Value.substr(Start,End-Start);
}

static std::string ToLowerAscii(const std::string &Value)
{
  std::string Lower=Value;
  for (size_t I=0;I<Lower.size();I++)
    Lower[I]=(char)std::tolower((unsigned char)Lower[I]);
  return Lower;
}

static std::string JoinPath(const std::string &Left,const std::string &Right)
{
  if (Left.empty() || Right.empty() || Right[0]=='/')
    return Left.empty() ? Right:Left;
  if (Left[Left.size()-1]=='/')
    return Left+Right;
  return Left+"/"+Right;
}

static std::string BaseName(const std::string &Path)
{
  size_t Pos=Path.find_last_of('/');
  return Pos==std::string::npos ? Path:Path.substr(Pos+1);
}

static bool LooksLikeTitleIdAt(const std::string &Text,size_t Pos)
{
  if (Pos+9>Text.size())
    return false;
  for (size_t I=0;I<4;I++)
    if (!std::isupper((unsigned char)Text[Pos+I]))
      return false;
  for (size_t I=4;I<9;I++)
    if (!std::isdigit((unsigned char)Text[Pos+I]))
      return false;
  return true;
}

static bool FindTitleIdInText(const std::string &Text,std::string &TitleId)
{
  for (size_t I=0;I<Text.size();I++)
  {
    if (LooksLikeTitleIdAt(Text,I))
    {
      TitleId=Text.substr(I,9);
      return true;
    }
  }
  return false;
}

static std::string DirName(const std::string &Path)
{
  size_t Pos=Path.find_last_of('/');
  if (Pos==std::string::npos)
    return ".";
  if (Pos==0)
    return "/";
  return Path.substr(0,Pos);
}

static bool PathExists(const std::string &Path);
static bool RemoveTree(const std::string &Path,std::string &Error);
static void LogLine(const char *Fmt,...);

enum NormalizeResult
{
  NORMALIZE_ERROR,
  NORMALIZE_INSTALLED,
  NORMALIZE_SKIPPED
};

enum ArchiveType
{
  ARCHIVE_UNKNOWN,
  ARCHIVE_RAR,
  ARCHIVE_7Z
};

static const char *ArchiveTypeName(ArchiveType Type)
{
  switch(Type)
  {
    case ARCHIVE_RAR: return "rar";
    case ARCHIVE_7Z: return "7z";
    default: return "unknown";
  }
}

static bool ShouldSkipExistingInstallBeforeExtract(const PayloadConfig &Cfg,
                                                   const std::string &ArchivePath,
                                                   std::string &TitleId,
                                                   std::string &FinalPath)
{
  if (!FindTitleIdInText(BaseName(ArchivePath),TitleId) &&
      !FindTitleIdInText(ArchivePath,TitleId))
    return false;

  FinalPath=JoinPath(Cfg.extract_location,TitleId+"-app");
  return PathExists(FinalPath);
}

static bool PathExists(const std::string &Path)
{
  struct stat St;
  return stat(Path.c_str(),&St)==0;
}

static bool IsDirPath(const std::string &Path)
{
  struct stat St;
  return stat(Path.c_str(),&St)==0 && S_ISDIR(St.st_mode);
}

static bool MkdirAll(const std::string &Path)
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

static bool ReadSmallFile(const std::string &Path,std::string &Data,size_t MaxSize=1024*1024)
{
  int Fd=open(Path.c_str(),O_RDONLY);
  if (Fd<0)
    return false;

  Data.clear();
  char Buf[4096];
  while (Data.size()<MaxSize)
  {
    ssize_t ReadSize=read(Fd,Buf,sizeof(Buf));
    if (ReadSize<0)
    {
      close(Fd);
      return false;
    }
    if (ReadSize==0)
      break;
    Data.append(Buf,(size_t)ReadSize);
  }
  close(Fd);
  return true;
}

static bool ReadFilePrefix(const std::string &Path,unsigned char *Buf,size_t BufSize,size_t &ReadSize)
{
  ReadSize=0;
  int Fd=open(Path.c_str(),O_RDONLY);
  if (Fd<0)
    return false;
  ssize_t Got=read(Fd,Buf,BufSize);
  close(Fd);
  if (Got<0)
    return false;
  ReadSize=(size_t)Got;
  return true;
}

static ArchiveType DetectArchiveType(const std::string &Path)
{
  unsigned char Magic[8];
  size_t MagicSize=0;
  if (ReadFilePrefix(Path,Magic,sizeof(Magic),MagicSize))
  {
    if (MagicSize>=7 &&
        Magic[0]==0x52 && Magic[1]==0x61 && Magic[2]==0x72 && Magic[3]==0x21 &&
        Magic[4]==0x1a && Magic[5]==0x07 && (Magic[6]==0x00 || Magic[6]==0x01))
      return ARCHIVE_RAR;
    if (MagicSize>=6 &&
        Magic[0]==0x37 && Magic[1]==0x7a && Magic[2]==0xbc &&
        Magic[3]==0xaf && Magic[4]==0x27 && Magic[5]==0x1c)
      return ARCHIVE_7Z;
  }

  std::string Name=ToLowerAscii(BaseName(Path));
  if (EndsWithNoCase(Name,".rar") ||
      (Name.size()>4 && Name[Name.size()-4]=='.' && Name[Name.size()-3]=='r' &&
       std::isdigit((unsigned char)Name[Name.size()-2]) &&
       std::isdigit((unsigned char)Name[Name.size()-1])))
    return ARCHIVE_RAR;
  if (EndsWithNoCase(Name,".7z") || EndsWithNoCase(Name,".7z.001"))
    return ARCHIVE_7Z;
  return ARCHIVE_UNKNOWN;
}

static bool WriteTextFile(const std::string &Path,const char *Data)
{
  int Fd=open(Path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);
  if (Fd<0)
    return false;

  size_t Len=strlen(Data);
  const char *Ptr=Data;
  while (Len>0)
  {
    ssize_t Written=write(Fd,Ptr,Len);
    if (Written<=0)
    {
      close(Fd);
      return false;
    }
    Ptr+=Written;
    Len-=(size_t)Written;
  }
  close(Fd);
  return true;
}

static bool IsKnownConfigKey(const std::string &Key)
{
  return Key=="filename" || Key=="archive_location" || Key=="archive_password" ||
         Key=="rar_location" || Key=="rar_password" ||
         Key=="delete_after" || Key=="extract_location" || Key=="threads" ||
         Key=="nice" || Key=="cpu_mask" || Key=="progress";
}

static bool ConfigLooksValid(const std::string &Path)
{
  std::string Data;
  if (!ReadSmallFile(Path,Data))
    return false;

  size_t Pos=0;
  while (Pos<Data.size())
  {
    size_t End=Data.find('\n',Pos);
    std::string Line=Data.substr(Pos,End==std::string::npos ? std::string::npos:End-Pos);
    Pos=End==std::string::npos ? Data.size():End+1;

    Line=Trim(Line);
    if (Line.empty() || Line[0]=='#' || Line[0]==';')
      continue;

    size_t Eq=Line.find('=');
    if (Eq==std::string::npos)
      continue;

    if (IsKnownConfigKey(Trim(Line.substr(0,Eq))))
      return true;
  }
  return false;
}

static void Notify(const char *Fmt,...)
{
  notify_request_t Req;
  memset(&Req,0,sizeof(Req));

  va_list Args;
  va_start(Args,Fmt);
  vsnprintf(Req.message,sizeof(Req.message),Fmt,Args);
  va_end(Args);

  sceKernelSendNotificationRequest(0,&Req,sizeof(Req),0);
}

static void LogLine(const char *Fmt,...)
{
  if (!MkdirAll(UNRAR_DATA_DIR))
    return;

  int Fd=open(UNRAR_DATA_DIR "/unrar.log",O_WRONLY|O_CREAT|O_APPEND,0666);
  if (Fd<0)
    return;

  char Msg[2048];
  va_list Args;
  va_start(Args,Fmt);
  int Len=vsnprintf(Msg,sizeof(Msg),Fmt,Args);
  va_end(Args);

  if (Len>0)
  {
    size_t Size=(size_t)std::min(Len,(int)sizeof(Msg)-2);
    Msg[Size++]='\n';
    write(Fd,Msg,Size);
  }
  close(Fd);
}

static uint64 NowMs()
{
  struct timespec Ts;
  if (clock_gettime(CLOCK_MONOTONIC,&Ts)!=0)
    return 0;
  return (uint64)Ts.tv_sec*1000+(uint64)Ts.tv_nsec/1000000;
}

extern "C" void Ps5UnrarProgress(int Percent)
{
  if (Percent<0)
    return;
  if (Percent>100)
    Percent=100;

  int Interval=ProgressInterval<1 ? 10:ProgressInterval;
  int Bucket=(Percent/Interval)*Interval;
  if (Percent==100)
    Bucket=100;

  if (Bucket>=LastNotifiedProgress+Interval || Bucket==100 && LastNotifiedProgress<100)
  {
    LastNotifiedProgress=Bucket;
    Notify("UnRAR: extraction %d%%",Bucket);
  }
}

static void AddUsbConfigRoot(const std::string &UsbRoot,std::vector<std::string> &Roots)
{
  if (!IsDirPath(UsbRoot))
    return;

  Roots.push_back(UsbRoot);
}

static void CollectUsbConfigRoots(std::vector<std::string> &Roots)
{
  AddUsbConfigRoot("/mnt/usb0",Roots);
  AddUsbConfigRoot("/mnt/usb1",Roots);
  AddUsbConfigRoot("/mnt/usb2",Roots);
  AddUsbConfigRoot("/mnt/usb3",Roots);
  AddUsbConfigRoot("/mnt/usb4",Roots);
  AddUsbConfigRoot("/mnt/usb5",Roots);
  AddUsbConfigRoot("/mnt/usb6",Roots);
  AddUsbConfigRoot("/mnt/usb7",Roots);
  AddUsbConfigRoot("/mnt/usb",Roots);
  AddUsbConfigRoot("/usb0",Roots);
  AddUsbConfigRoot("/usb1",Roots);
}

static std::string SelectNamedConfigPath(const std::string &ConfigName)
{
  std::vector<std::string> Roots;
  CollectUsbConfigRoots(Roots);

  for (size_t I=0;I<Roots.size();I++)
  {
    std::string Path=JoinPath(Roots[I],"unrar/"+ConfigName);
    if (ConfigLooksValid(Path))
      return Path;
  }

  for (size_t I=0;I<Roots.size();I++)
  {
    std::string Path=JoinPath(Roots[I],ConfigName);
    if (ConfigLooksValid(Path))
      return Path;
  }

  std::string InternalPath=JoinPath(UNRAR_DATA_DIR,ConfigName);
  if (ConfigLooksValid(InternalPath))
    return InternalPath;

  return "";
}

static std::string SelectConfigPath()
{
  std::string Path=SelectNamedConfigPath("config.ini");
  if (!Path.empty())
    return Path;

  return UNRAR_CONFIG;
}

static bool EnsureDefaultConfig(const std::string &ConfigPath,std::string &Error)
{
  if (!MkdirAll(UNRAR_DATA_DIR))
  {
    Error="failed to create " UNRAR_DATA_DIR;
    return false;
  }

  if (ConfigPath!=UNRAR_CONFIG || PathExists(UNRAR_CONFIG))
    return true;

  static const char DefaultConfig[]=
    "filename=\n"
    "archive_location=/data/unrar\n"
    "archive_password=\n"
    "delete_after=0\n"
    "extract_location=/data/homebrew\n"
    "threads=0\n"
    "nice=-20\n"
    "cpu_mask=0\n"
    "progress=10\n";

  if (!WriteTextFile(UNRAR_CONFIG,DefaultConfig))
  {
    Error="failed to create " UNRAR_CONFIG;
    return false;
  }
  return true;
}

static void ApplyConfigData(PayloadConfig &Cfg,const std::string &Data,bool AllowFilenames)
{
  size_t Pos=0;
  while (Pos<Data.size())
  {
    size_t End=Data.find('\n',Pos);
    std::string Line=Data.substr(Pos,End==std::string::npos ? std::string::npos:End-Pos);
    Pos=End==std::string::npos ? Data.size():End+1;

    Line=Trim(Line);
    if (Line.empty() || Line[0]=='#' || Line[0]==';')
      continue;

    size_t Eq=Line.find('=');
    if (Eq==std::string::npos)
      continue;

    std::string Key=Trim(Line.substr(0,Eq));
    std::string Value=Trim(Line.substr(Eq+1));
    if (Key=="filename")
    {
      if (AllowFilenames && !Value.empty())
        Cfg.filenames.push_back(Value);
    }
    else if ((Key=="archive_location" || Key=="rar_location") && !Value.empty())
      Cfg.archive_location=Value;
    else if (Key=="archive_password" || Key=="rar_password")
      Cfg.archive_password=Value;
    else if (Key=="delete_after")
      Cfg.delete_after=Value=="1" || Value=="true" || Value=="yes" || Value=="on";
    else if (Key=="extract_location" && !Value.empty())
      Cfg.extract_location=Value;
    else if (Key=="threads")
      Cfg.threads=(uint)strtoul(Value.c_str(),NULL,10);
    else if (Key=="nice")
      Cfg.nice=atoi(Value.c_str());
    else if (Key=="cpu_mask")
      Cfg.cpu_mask=strtoull(Value.c_str(),NULL,0);
    else if (Key=="progress")
    {
      Cfg.progress=atoi(Value.c_str());
      if (Cfg.progress<1)
        Cfg.progress=1;
      if (Cfg.progress>100)
        Cfg.progress=100;
    }
  }
}

static bool LoadConfig(PayloadConfig &Cfg,std::string &Error)
{
  Cfg.filenames.clear();
  Cfg.config_path=SelectConfigPath();
  Cfg.archive_location=UNRAR_DATA_DIR;
  Cfg.archive_password.clear();
  Cfg.delete_after=false;
  Cfg.extract_location=DEFAULT_EXTRACT_LOCATION;
  Cfg.threads=0;
  Cfg.nice=-20;
  Cfg.cpu_mask=0;
  Cfg.progress=10;

  if (!EnsureDefaultConfig(Cfg.config_path,Error))
    return false;

  std::string Data;
  if (!ReadSmallFile(Cfg.config_path,Data))
  {
    Error="failed to read "+Cfg.config_path;
    return false;
  }

  ApplyConfigData(Cfg,Data,true);
  return true;
}

static bool ApplyConfigFile(PayloadConfig &Cfg,const std::string &ConfigPath,
                            bool AllowFilenames,std::string &Error)
{
  std::string Data;
  if (!ReadSmallFile(ConfigPath,Data))
  {
    Error="failed to read "+ConfigPath;
    return false;
  }
  ApplyConfigData(Cfg,Data,AllowFilenames);
  return true;
}

static void ApplySchedulingConfig(const PayloadConfig &Cfg)
{
  if (Cfg.nice>=-20 && Cfg.nice<=20)
  {
    if (setpriority(PRIO_PROCESS,0,Cfg.nice)==0)
      LogLine("priority nice=%d result=ok",Cfg.nice);
    else
      LogLine("priority nice=%d result=fail errno=%d",Cfg.nice,errno);
  }

  if (Cfg.cpu_mask!=0)
  {
    cpuset_t Mask;
    CPU_ZERO(&Mask);
    for (uint Cpu=0;Cpu<64;Cpu++)
      if ((Cfg.cpu_mask & ((uint64)1<<Cpu))!=0)
        CPU_SET(Cpu,&Mask);

    if (!HaveOriginalCpuAffinity &&
        cpuset_getaffinity(CPU_LEVEL_WHICH,CPU_WHICH_PID,getpid(),
                           sizeof(OriginalCpuAffinity),&OriginalCpuAffinity)==0)
      HaveOriginalCpuAffinity=true;

    if (cpuset_setaffinity(CPU_LEVEL_WHICH,CPU_WHICH_PID,getpid(),sizeof(Mask),&Mask)==0)
    {
      CpuAffinityApplied=true;
      LogLine("cpu_mask=0x%llx result=ok",(unsigned long long)Cfg.cpu_mask);
    }
    else
      LogLine("cpu_mask=0x%llx result=fail errno=%d",(unsigned long long)Cfg.cpu_mask,errno);
  }
  else if (CpuAffinityApplied && HaveOriginalCpuAffinity)
  {
    if (cpuset_setaffinity(CPU_LEVEL_WHICH,CPU_WHICH_PID,getpid(),
                           sizeof(OriginalCpuAffinity),&OriginalCpuAffinity)==0)
    {
      CpuAffinityApplied=false;
      LogLine("cpu_mask=0x0 result=ok");
    }
    else
      LogLine("cpu_mask=0x0 result=fail errno=%d",errno);
  }
}

static bool IsDiscoverableArchiveName(const std::string &Name)
{
  std::string Lower=ToLowerAscii(Name);
  return EndsWithNoCase(Lower,".rar") ||
         EndsWithNoCase(Lower,".7z") ||
         EndsWithNoCase(Lower,".7z.001");
}

static void CollectArchiveFiles(const std::string &Root,std::vector<std::string> &Paths,int Depth=0)
{
  if (Depth>8 || BaseName(Root)==STAGING_DIR_NAME)
    return;

  DIR *Dir=opendir(Root.c_str());
  if (Dir==NULL)
    return;

  for (;;)
  {
    struct dirent *Entry=readdir(Dir);
    if (Entry==NULL)
      break;

    std::string Name=Entry->d_name;
    if (Name=="." || Name==".." || (!Name.empty() && Name[0]=='.'))
      continue;

    std::string FullPath=JoinPath(Root,Name);
    struct stat St;
    if (lstat(FullPath.c_str(),&St)!=0)
      continue;

    if (S_ISDIR(St.st_mode))
    {
      CollectArchiveFiles(FullPath,Paths,Depth+1);
      continue;
    }

    if (S_ISREG(St.st_mode) && IsDiscoverableArchiveName(Name))
      Paths.push_back(FullPath);
  }
  closedir(Dir);
}

static bool FindArchiveFiles(const std::string &ArchiveLocation,std::vector<std::string> &Paths,
                             std::string &Error)
{
  if (!IsDirPath(ArchiveLocation))
  {
    Error="failed to open archive_location: "+ArchiveLocation;
    return false;
  }

  Paths.clear();
  CollectArchiveFiles(ArchiveLocation,Paths);

  if (Paths.empty())
  {
    Error="no .rar or .7z file found under "+ArchiveLocation;
    return false;
  }

  std::sort(Paths.begin(),Paths.end());
  return true;
}

static bool ResolveConfiguredArchivePaths(const PayloadConfig &Cfg,std::vector<std::string> &ArchivePaths,std::string &Error)
{
  ArchivePaths.clear();
  if (Cfg.filenames.empty())
  {
    if (!FindArchiveFiles(Cfg.archive_location,ArchivePaths,Error))
      return false;
    return true;
  }

  for (size_t I=0;I<Cfg.filenames.size();I++)
  {
    const std::string &Filename=Cfg.filenames[I];
    if (Filename.empty())
      continue;

    std::string ArchivePath=Filename[0]=='/' ? Filename:JoinPath(Cfg.archive_location,Filename);
    if (!PathExists(ArchivePath))
    {
      Error="archive not found: "+ArchivePath;
      return false;
    }
    ArchivePaths.push_back(ArchivePath);
  }

  if (ArchivePaths.empty())
  {
    Error="no archive configured";
    return false;
  }
  return true;
}

static bool RemoveTree(const std::string &Path,std::string &Error)
{
  struct stat St;
  if (lstat(Path.c_str(),&St)!=0)
    return errno==ENOENT;

  if (S_ISDIR(St.st_mode))
  {
    DIR *Dir=opendir(Path.c_str());
    if (Dir==NULL)
    {
      Error="failed to open directory: "+Path;
      return false;
    }
    for (;;)
    {
      struct dirent *Entry=readdir(Dir);
      if (Entry==NULL)
        break;
      std::string Name=Entry->d_name;
      if (Name=="." || Name=="..")
        continue;
      if (!RemoveTree(JoinPath(Path,Name),Error))
      {
        closedir(Dir);
        return false;
      }
    }
    closedir(Dir);
    if (rmdir(Path.c_str())!=0)
    {
      Error="failed to remove directory: "+Path;
      return false;
    }
    return true;
  }

  if (unlink(Path.c_str())!=0)
  {
    Error="failed to remove file: "+Path;
    return false;
  }
  return true;
}

static bool MovePath(const std::string &From,const std::string &To,std::string &Error)
{
  if (rename(From.c_str(),To.c_str())==0)
    return true;

  Error="failed to move "+From+" to "+To;
  return false;
}

static bool MoveDirectoryContents(const std::string &From,const std::string &To,std::string &Error)
{
  if (!MkdirAll(To))
  {
    Error="failed to create "+To;
    return false;
  }

  DIR *Dir=opendir(From.c_str());
  if (Dir==NULL)
  {
    Error="failed to open staging directory";
    return false;
  }

  for (;;)
  {
    struct dirent *Entry=readdir(Dir);
    if (Entry==NULL)
      break;
    std::string Name=Entry->d_name;
    if (Name=="." || Name=="..")
      continue;
    if (!MovePath(JoinPath(From,Name),JoinPath(To,Name),Error))
    {
      closedir(Dir);
      return false;
    }
  }
  closedir(Dir);
  return true;
}

static bool FindParamJson(const std::string &Root,std::string &ParamPath,int Depth=0)
{
  if (Depth>8)
    return false;

  std::string Candidate=JoinPath(Root,"sce_sys/param.json");
  if (PathExists(Candidate))
  {
    ParamPath=Candidate;
    return true;
  }

  DIR *Dir=opendir(Root.c_str());
  if (Dir==NULL)
    return false;

  for (;;)
  {
    struct dirent *Entry=readdir(Dir);
    if (Entry==NULL)
      break;
    std::string Name=Entry->d_name;
    if (Name=="." || Name=="..")
      continue;
    std::string Child=JoinPath(Root,Name);
    if (IsDirPath(Child) && FindParamJson(Child,ParamPath,Depth+1))
    {
      closedir(Dir);
      return true;
    }
  }
  closedir(Dir);
  return false;
}

static bool ExtractJsonString(const std::string &Json,const char *Key,std::string &Value)
{
  std::string Needle="\"";
  Needle+=Key;
  Needle+="\"";

  size_t Pos=Json.find(Needle);
  if (Pos==std::string::npos)
    return false;
  Pos=Json.find(':',Pos+Needle.size());
  if (Pos==std::string::npos)
    return false;
  Pos=Json.find('"',Pos+1);
  if (Pos==std::string::npos)
    return false;

  size_t End=Pos+1;
  std::string Out;
  while (End<Json.size())
  {
    char Ch=Json[End++];
    if (Ch=='\\' && End<Json.size())
    {
      Out+=Json[End++];
      continue;
    }
    if (Ch=='"')
    {
      Value=Out;
      return true;
    }
    Out+=Ch;
  }
  return false;
}

static bool ReadTitleId(const std::string &ParamPath,std::string &TitleId,std::string &Error)
{
  std::string Json;
  if (!ReadSmallFile(ParamPath,Json))
  {
    Error="failed to read "+ParamPath;
    return false;
  }

  if (ExtractJsonString(Json,"titleId",TitleId) ||
      ExtractJsonString(Json,"title_id",TitleId) ||
      ExtractJsonString(Json,"TITLE_ID",TitleId))
    return true;

  Error="TitleID not found in "+ParamPath;
  return false;
}

static const char *ExitReason(RAR_EXIT Code)
{
  switch(Code)
  {
    case RARX_SUCCESS: return "success";
    case RARX_WARNING: return "warning";
    case RARX_FATAL: return "fatal extraction error";
    case RARX_CRC: return "checksum or password error";
    case RARX_LOCK: return "archive lock error";
    case RARX_WRITE: return "write error";
    case RARX_OPEN: return "open error";
    case RARX_USERERROR: return "bad command or config";
    case RARX_MEMORY: return "out of memory";
    case RARX_CREATE: return "file create error";
    case RARX_NOFILES: return "no files extracted";
    case RARX_BADPWD: return "bad password";
    case RARX_READ: return "read error";
    case RARX_BADARC: return "bad archive";
    case RARX_USERBREAK: return "user break";
  }
  return "unknown error";
}

static int RunUnrarExtract(const std::string &ArchivePath,const std::string &DestPath,
                           const std::string &Password,uint Threads)
{
  ErrHandler.Clean();
  ErrHandler.SetSignalHandlers(true);

  std::unique_ptr<CommandData> Cmd(new CommandData);
  Cmd->Command=L"X";
  CharToWide(ArchivePath,Cmd->ArcName);
  CharToWide(DestPath,Cmd->ExtrPath);
  AddEndSlash(Cmd->ExtrPath);
  Cmd->AddArcName(Cmd->ArcName);
  Cmd->FileArgs.AddString(MASKALL);
  Cmd->AllYes=true;
  Cmd->Overwrite=OVERWRITE_ALL;
  /* Windows/read-only archive attributes must not remove execute/search bits
   * from PS5 game dumps. The caller normalizes every format before publish. */
  Cmd->IgnoreGeneralAttr=true;
  Cmd->DisableCopyright=true;
  Cmd->DisableDone=true;
  Cmd->DisableNames=true;
  if (Threads>0)
    Cmd->Threads=std::min(Threads,MaxPoolThreads);

  if (!Password.empty())
  {
    std::wstring PasswordW;
    CharToWide(Password,PasswordW);
    Cmd->Password.Set(PasswordW.c_str());
  }

  uiInit(SOUND_NOTIFY_OFF);

  try
  {
    CmdExtract Extract(Cmd.get());
    Extract.DoExtract();
  }
  catch (RAR_EXIT ErrCode)
  {
    ErrHandler.SetErrorCode(ErrCode);
  }
  catch (std::bad_alloc&)
  {
    ErrHandler.SetErrorCode(RARX_MEMORY);
  }
  catch (std::length_error&)
  {
    ErrHandler.SetErrorCode(RARX_MEMORY);
  }
  catch (...)
  {
    ErrHandler.SetErrorCode(RARX_FATAL);
  }

  ErrHandler.MainExit=true;
  return ErrHandler.GetErrorCode();
}

static NormalizeResult NormalizeExtractedApp(const PayloadConfig &Cfg,const std::string &StagePath,
                                             std::string &TitleId,
                                             std::string &FinalPath,std::string &Error)
{
  std::string ParamPath;
  if (!FindParamJson(StagePath,ParamPath))
  {
    Error="sce_sys/param.json not found after extraction";
    return NORMALIZE_ERROR;
  }

  if (!ReadTitleId(ParamPath,TitleId,Error))
    return NORMALIZE_ERROR;

  FinalPath=JoinPath(Cfg.extract_location,TitleId+"-app");
  std::string ExistingRoot=JoinPath(StagePath,TitleId+"-app");
  std::string ParamRoot=DirName(DirName(ParamPath));

  if (!MkdirAll(Cfg.extract_location))
  {
    Error="failed to create "+Cfg.extract_location;
    return NORMALIZE_ERROR;
  }

  if (PathExists(FinalPath))
    return NORMALIZE_SKIPPED;

  if (PathExists(ExistingRoot))
    return MovePath(ExistingRoot,FinalPath,Error) ? NORMALIZE_INSTALLED:NORMALIZE_ERROR;

  if (ParamRoot!=StagePath)
    return MovePath(ParamRoot,FinalPath,Error) ? NORMALIZE_INSTALLED:NORMALIZE_ERROR;

  return MoveDirectoryContents(StagePath,FinalPath,Error) ? NORMALIZE_INSTALLED:NORMALIZE_ERROR;
}

static std::string GetArchiveStem(const std::string &Name)
{
  std::string Stem=Name;
  std::string Lower=ToLowerAscii(Name);

  if (EndsWithNoCase(Lower,".7z.001"))
  {
    Stem.erase(Stem.size()-7);
    Lower.erase(Lower.size()-7);
  }
  else if (EndsWithNoCase(Lower,".7z"))
  {
    Stem.erase(Stem.size()-3);
    Lower.erase(Lower.size()-3);
  }
  else if (EndsWithNoCase(Stem,".rar"))
  {
    Stem.erase(Stem.size()-4);
    Lower.erase(Lower.size()-4);
  }
  else if (Lower.size()>4 && Lower[Lower.size()-4]=='.' &&
           Lower[Lower.size()-3]=='r' &&
           std::isdigit((unsigned char)Lower[Lower.size()-2]) &&
           std::isdigit((unsigned char)Lower[Lower.size()-1]))
  {
    Stem.erase(Stem.size()-4);
    Lower.erase(Lower.size()-4);
  }

  size_t PartPos=Lower.rfind(".part");
  if (PartPos!=std::string::npos && PartPos+5<Lower.size())
  {
    bool PartNumber=true;
    for (size_t I=PartPos+5;I<Lower.size();I++)
      if (!std::isdigit((unsigned char)Lower[I]))
      {
        PartNumber=false;
        break;
      }
    if (PartNumber)
      Stem.erase(PartPos);
  }

  return Stem;
}

static bool ApplySidecarConfigForArchive(PayloadConfig &Cfg,const std::string &ArchivePath,
                                         std::string &SidecarConfigPath,std::string &Error)
{
  std::string Path=JoinPath(DirName(ArchivePath),GetArchiveStem(BaseName(ArchivePath))+".ini");
  if (!ConfigLooksValid(Path))
  {
    std::string TitleId;
    if (!FindTitleIdInText(ArchivePath,TitleId))
      return true;

    Path=JoinPath(DirName(ArchivePath),TitleId+".ini");
    if (!ConfigLooksValid(Path))
    {
      Path=SelectNamedConfigPath(TitleId+".ini");
      if (Path.empty())
        return true;
    }
  }

  if (!ApplyConfigFile(Cfg,Path,false,Error))
    return false;

  SidecarConfigPath=Path;
  return true;
}

static bool IsArchivePartName(const std::string &Name,const std::string &Stem)
{
  std::string LowerName=ToLowerAscii(Name);
  std::string LowerStem=ToLowerAscii(Stem);

  if (LowerName==LowerStem+".rar")
    return true;

  if (LowerName.find(LowerStem+".part")==0 && EndsWithNoCase(LowerName,".rar"))
  {
    size_t DigitsStart=LowerStem.size()+5;
    size_t DigitsEnd=LowerName.size()-4;
    if (DigitsStart<DigitsEnd)
    {
      for (size_t I=DigitsStart;I<DigitsEnd;I++)
        if (!std::isdigit((unsigned char)LowerName[I]))
          return false;
      return true;
    }
  }

  if (LowerName.size()==LowerStem.size()+4 && LowerName.compare(0,LowerStem.size(),LowerStem)==0 &&
      LowerName[LowerStem.size()]=='.' && LowerName[LowerStem.size()+1]=='r' &&
      std::isdigit((unsigned char)LowerName[LowerStem.size()+2]) &&
      std::isdigit((unsigned char)LowerName[LowerStem.size()+3]))
    return true;

  return false;
}

static bool IsOldStylePartName(const std::string &Name)
{
  std::string Lower=ToLowerAscii(Name);
  size_t Dot=Lower.find_last_of('.');
  return Dot!=std::string::npos && Lower.size()==Dot+4 && Lower[Dot+1]=='r' &&
         std::isdigit((unsigned char)Lower[Dot+2]) &&
         std::isdigit((unsigned char)Lower[Dot+3]);
}

static bool IsNumberedPartRarName(const std::string &Name)
{
  std::string Lower=ToLowerAscii(Name);
  if (!EndsWithNoCase(Lower,".rar"))
    return false;

  size_t PartPos=Lower.rfind(".part");
  if (PartPos==std::string::npos || PartPos+5>=Lower.size()-4)
    return false;

  for (size_t I=PartPos+5;I<Lower.size()-4;I++)
    if (!std::isdigit((unsigned char)Lower[I]))
      return false;
  return true;
}

static int PartNumberFromName(const std::string &Name)
{
  std::string Lower=ToLowerAscii(Name);
  size_t PartPos=Lower.rfind(".part");
  if (PartPos==std::string::npos)
    return 0;
  return atoi(Lower.c_str()+PartPos+5);
}

static bool Is7zVolumeName(const std::string &Name)
{
  std::string Lower=ToLowerAscii(Name);
  if (Lower.size()<8)
    return false;
  size_t Dot=Lower.size()-4;
  return Lower[Dot]=='.' &&
         std::isdigit((unsigned char)Lower[Dot+1]) &&
         std::isdigit((unsigned char)Lower[Dot+2]) &&
         std::isdigit((unsigned char)Lower[Dot+3]) &&
         EndsWithNoCase(Lower.substr(0,Dot),".7z");
}

static std::string First7zVolumePath(const std::string &ArchivePath)
{
  std::string Name=BaseName(ArchivePath);
  if (!Is7zVolumeName(Name))
    return ArchivePath;
  std::string Prefix=Name.substr(0,Name.size()-3);
  std::string First=JoinPath(DirName(ArchivePath),Prefix+"001");
  return PathExists(First) ? First:ArchivePath;
}

static std::string ArchiveGroupKey(const std::string &ArchivePath)
{
  ArchiveType Type=DetectArchiveType(ArchivePath);
  return std::string(ArchiveTypeName(Type))+":"+ToLowerAscii(DirName(ArchivePath)+"/"+GetArchiveStem(BaseName(ArchivePath)));
}

static std::string PreferredArchiveStart(const std::string &ArchivePath)
{
  if (Is7zVolumeName(BaseName(ArchivePath)))
    return First7zVolumePath(ArchivePath);

  ArchiveType Type=DetectArchiveType(ArchivePath);
  if (Type==ARCHIVE_7Z)
    return First7zVolumePath(ArchivePath);

  std::string Dir=DirName(ArchivePath);
  std::string Name=BaseName(ArchivePath);
  std::string Stem=GetArchiveStem(Name);
  std::string PlainRar=JoinPath(Dir,Stem+".rar");
  std::string Part1=JoinPath(Dir,Stem+".part1.rar");
  std::string Part01=JoinPath(Dir,Stem+".part01.rar");

  if (IsOldStylePartName(Name) && PathExists(PlainRar))
    return PlainRar;

  if (IsNumberedPartRarName(Name))
  {
    if (PathExists(Part1))
      return Part1;
    if (PathExists(Part01))
      return Part01;
    if (PartNumberFromName(Name)!=1)
      return ArchivePath;
  }

  return ArchivePath;
}

static void AddUniqueArchive(std::vector<std::string> &Archives,std::vector<std::string> &Keys,
                             const std::string &ArchivePath)
{
  std::string Preferred=PreferredArchiveStart(ArchivePath);
  std::string Key=ArchiveGroupKey(Preferred);
  for (size_t I=0;I<Keys.size();I++)
    if (Keys[I]==Key)
      return;
  Keys.push_back(Key);
  Archives.push_back(Preferred);
}

static bool BuildArchiveQueue(const PayloadConfig &Cfg,std::vector<std::string> &Archives,std::string &Error)
{
  std::vector<std::string> RawArchives;
  if (!ResolveConfiguredArchivePaths(Cfg,RawArchives,Error))
    return false;

  Archives.clear();
  std::vector<std::string> Keys;
  for (size_t I=0;I<RawArchives.size();I++)
    AddUniqueArchive(Archives,Keys,RawArchives[I]);

  if (Archives.empty())
  {
    Error="no archive selected";
    return false;
  }
  return true;
}

static bool Is7zPartNameForStem(const std::string &Name,const std::string &Stem)
{
  std::string LowerName=ToLowerAscii(Name);
  std::string LowerStem=ToLowerAscii(Stem);
  if (LowerName.find(LowerStem+".7z.")!=0)
    return false;
  if (LowerName.size()!=LowerStem.size()+7)
    return false;
  return std::isdigit((unsigned char)LowerName[LowerStem.size()+4]) &&
         std::isdigit((unsigned char)LowerName[LowerStem.size()+5]) &&
         std::isdigit((unsigned char)LowerName[LowerStem.size()+6]);
}

static void DeleteArchiveFiles(const std::string &ArchivePath,ArchiveType Type)
{
  std::string Dir=DirName(ArchivePath);
  std::string Name=BaseName(ArchivePath);
  std::string Stem=GetArchiveStem(Name);

  if (Type==ARCHIVE_7Z && !Is7zVolumeName(Name))
  {
    unlink(ArchivePath.c_str());
    return;
  }

  DIR *D=opendir(Dir.c_str());
  if (D==NULL)
    return;

  for (;;)
  {
    struct dirent *Entry=readdir(D);
    if (Entry==NULL)
      break;
    std::string Cur=Entry->d_name;
    if (Cur=="." || Cur=="..")
      continue;
    if ((Type==ARCHIVE_RAR && IsArchivePartName(Cur,Stem)) ||
        (Type==ARCHIVE_7Z && Is7zPartNameForStem(Cur,Stem)))
      unlink(JoinPath(Dir,Cur).c_str());
  }
  closedir(D);
}

static int ExtractArchive(const PayloadConfig &Cfg,const std::string &ArchivePath,
                          const std::string &SidecarConfigPath,size_t Index,size_t Total,
                          bool &Installed)
{
  std::string Error;
  Installed=false;
  std::string StagePath=JoinPath(Cfg.extract_location,STAGING_DIR_NAME);
  ArchiveType Type=DetectArchiveType(ArchivePath);
  if (Type==ARCHIVE_UNKNOWN)
  {
    Notify("Archive error: unsupported archive type");
    LogLine("archive_error archive=%s reason=unsupported_archive_type",ArchivePath.c_str());
    return RARX_USERERROR;
  }

  Notify("Archive: starting %u/%u %s",(unsigned)(Index+1),(unsigned)Total,BaseName(ArchivePath).c_str());
  LogLine("start archive=%s archive_type=%s config=%s sidecar_config=%s archive_location=%s extract_location=%s stage_path=%s delete_after=%u threads=%u nice=%d cpu_mask=0x%llx progress=%d",
          ArchivePath.c_str(),ArchiveTypeName(Type),Cfg.config_path.c_str(),
          SidecarConfigPath.empty() ? "none":SidecarConfigPath.c_str(),
          Cfg.archive_location.c_str(),Cfg.extract_location.c_str(),StagePath.c_str(),
          Cfg.delete_after ? 1:0,Cfg.threads,Cfg.nice,
          (unsigned long long)Cfg.cpu_mask,Cfg.progress);
  LastNotifiedProgress=-10;

  std::string ExistingTitleId;
  std::string ExistingFinalPath;
  if (ShouldSkipExistingInstallBeforeExtract(Cfg,ArchivePath,ExistingTitleId,ExistingFinalPath))
  {
    Notify("Archive skipped: %s already installed",ExistingTitleId.c_str());
    LogLine("skip archive=%s archive_type=%s title_id=%s final_path=%s reason=already_installed_precheck",
            ArchivePath.c_str(),ArchiveTypeName(Type),ExistingTitleId.c_str(),ExistingFinalPath.c_str());
    return RARX_SUCCESS;
  }

  if (!RemoveTree(StagePath,Error))
  {
    Notify("Archive error: %s",Error.c_str());
    LogLine("staging_error %s",Error.c_str());
    return 1;
  }
  if (!MkdirAll(StagePath))
  {
    Notify("Archive error: failed to create staging directory");
    LogLine("staging_error failed to create staging directory: %s",StagePath.c_str());
    return 1;
  }

  uint64 ExtractStart=NowMs();
  int Code=RARX_FATAL;
  std::string Reason;
  std::string MissingVolume;
  if (Type==ARCHIVE_RAR)
  {
    Code=RunUnrarExtract(ArchivePath,StagePath,Cfg.archive_password,Cfg.threads);
    Reason=ExitReason((RAR_EXIT)Code);
  }
  else
  {
    SevenZExtractResult SevenZ=Run7zExtract(ArchivePath,StagePath,Cfg.archive_password,Cfg.threads);
    Code=SevenZ.code;
    Reason=SevenZ.reason;
    MissingVolume=SevenZ.missing_volume;
  }
  uint64 ExtractMs=NowMs()-ExtractStart;
  LogLine("extract_result archive=%s archive_type=%s code=%d reason=%s elapsed_ms=%llu%s%s",
          ArchivePath.c_str(),ArchiveTypeName(Type),Code,Reason.c_str(),
          (unsigned long long)ExtractMs,
          MissingVolume.empty() ? "":" missing_volume=",
          MissingVolume.empty() ? "":MissingVolume.c_str());
  if (Code!=RARX_SUCCESS && Code!=RARX_WARNING)
  {
    Notify("Archive error: %s",Reason.c_str());
    return Code;
  }
  Ps5UnrarProgress(100);

  std::string TitleId;
  std::string FinalPath;
  uint64 NormalizeStart=NowMs();
  NormalizeResult Result=NormalizeExtractedApp(Cfg,StagePath,TitleId,FinalPath,Error);
  if (Result==NORMALIZE_ERROR)
  {
    Notify("Archive error: %s",Error.c_str());
    LogLine("normalize_error %s",Error.c_str());
    return 1;
  }
  uint64 NormalizeMs=NowMs()-NormalizeStart;

  RemoveTree(StagePath,Error);

  if (Result==NORMALIZE_SKIPPED)
  {
    Notify("Archive skipped: %s already installed",TitleId.c_str());
    LogLine("skip archive=%s archive_type=%s title_id=%s final_path=%s extract_ms=%llu normalize_ms=%llu reason=already_installed",
            ArchivePath.c_str(),ArchiveTypeName(Type),TitleId.c_str(),FinalPath.c_str(),
            (unsigned long long)ExtractMs,(unsigned long long)NormalizeMs);
    return Code;
  }

  if (Cfg.delete_after)
    DeleteArchiveFiles(ArchivePath,Type);

  Notify("Archive done: %s installed to %s",TitleId.c_str(),FinalPath.c_str());
  LogLine("done archive=%s archive_type=%s title_id=%s final_path=%s extract_ms=%llu normalize_ms=%llu",
          ArchivePath.c_str(),ArchiveTypeName(Type),TitleId.c_str(),FinalPath.c_str(),
          (unsigned long long)ExtractMs,(unsigned long long)NormalizeMs);
  Installed=true;
  return Code;
}

int main(int argc,char *argv[])
{
#ifdef _UNIX
  setlocale(LC_ALL,"");
#endif

  if (!MkdirAll(UNRAR_DATA_DIR))
  {
    Notify("UnRAR error: failed to create " UNRAR_DATA_DIR);
    return 1;
  }

  LockGuard Lock;
  if (!Lock.Acquire())
  {
    Notify("UnRAR: extraction already running");
    LogLine("lock_error extraction already running");
    return 1;
  }

  PayloadConfig Cfg;
  std::string Error;
  if (!LoadConfig(Cfg,Error))
  {
    Notify("UnRAR error: %s",Error.c_str());
    LogLine("config_error %s",Error.c_str());
    return 1;
  }

  bool AutoMode=Cfg.filenames.empty();
  std::vector<std::string> Archives;
  if (!BuildArchiveQueue(Cfg,Archives,Error))
  {
    Notify("UnRAR error: %s",Error.c_str());
    LogLine("archive_error %s",Error.c_str());
    return 1;
  }

  if (argc>1 && argv[1]!=NULL && argv[1][0]!=0)
  {
    AutoMode=false;
    Archives.clear();
    std::vector<std::string> Keys;
    AddUniqueArchive(Archives,Keys,argv[1]);
  }

  ProgressInterval=Cfg.progress;

  LogLine("queue config=%s archives=%u progress=%d",Cfg.config_path.c_str(),
          (unsigned)Archives.size(),Cfg.progress);
  int FinalCode=RARX_SUCCESS;
  bool AnyInstalled=false;
  for (size_t I=0;I<Archives.size();I++)
  {
    PayloadConfig ArchiveCfg=Cfg;
    std::string SidecarConfigPath;
    if (!ApplySidecarConfigForArchive(ArchiveCfg,Archives[I],SidecarConfigPath,Error))
    {
      Notify("UnRAR error: %s",Error.c_str());
      LogLine("sidecar_config_error archive=%s error=%s",Archives[I].c_str(),Error.c_str());
      return 1;
    }

    ApplySchedulingConfig(ArchiveCfg);
    ProgressInterval=ArchiveCfg.progress;
    bool Installed=false;
    int Code=ExtractArchive(ArchiveCfg,Archives[I],SidecarConfigPath,I,Archives.size(),
                            Installed);
    if (Code!=RARX_SUCCESS && Code!=RARX_WARNING)
      return Code;
    if (Code==RARX_WARNING)
      FinalCode=Code;
    if (Installed)
    {
      AnyInstalled=true;
      if (AutoMode)
        break;
    }
  }

  if (AutoMode && !AnyInstalled)
  {
    Notify("UnRAR: no new archives to extract");
    LogLine("queue_result reason=no_new_archives");
  }

  return FinalCode;
}
