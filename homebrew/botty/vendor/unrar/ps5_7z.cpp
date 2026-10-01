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
#include <unistd.h>
#include <vector>

extern "C" void Ps5UnrarProgress(int Percent);

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
      return false;
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
  ssize_t ReadSize=read(Fd,Data,SizeToRead);
  if (ReadSize<0)
    return ErrnoToHresult();
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
    ssize_t ReadSize=read(CurrentFd,(Byte *)Data+Done,Cur);
    if (ReadSize<0)
      return ErrnoToHresult();
    if (ReadSize==0)
      break;
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

public:
  CPosixOutStream():Fd(-1) {}
  ~CPosixOutStream()
  {
    if (Fd>=0)
      close(Fd);
  }

  bool Create(const std::string &Path)
  {
    Fd=open(Path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);
    return Fd>=0;
  }
};

Z7_COM7F_IMF(CPosixOutStream::Write(const void *Data,UInt32 SizeToWrite,UInt32 *ProcessedSize))
{
  if (ProcessedSize)
    *ProcessedSize=0;
  if (SizeToWrite==0)
    return S_OK;
  ssize_t Written=write(Fd,Data,SizeToWrite);
  if (Written<0)
    return ErrnoToHresult();
  if (ProcessedSize)
    *ProcessedSize=(UInt32)Written;
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

  std::string Parent=DirName7z(FullPath);
  if (!MkdirAll7z(Parent))
    return E_FAIL;

  CPosixOutStream *StreamSpec=new CPosixOutStream;
  CMyComPtr<ISequentialOutStream> Stream=StreamSpec;
  if (!StreamSpec->Create(FullPath))
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

  if (Threads>0)
  {
    UInt32 Clamped=std::min<uint>(std::max<uint>(Threads,1),8);
    const wchar_t *Names[2]={ L"mt", L"mtf" };
    NCOM::CPropVariant Values[2];
    Values[0]=(UInt32)Clamped;
    Values[1]=true;
    SetProperties->SetProperties(Names,Values,2);
  }
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
    Result.code=OpenResult==E_OUTOFMEMORY ? RARX_MEMORY:RARX_OPEN;
    Result.reason=OpenResult==E_OUTOFMEMORY ? "out of memory":"missing 7z volume or open error";
    if (VolumeStreamSpec && !VolumeStreamSpec->MissingVolume.empty())
      Result.missing_volume=VolumeStreamSpec->MissingVolume;
    else if (!OpenCallbackSpec->MissingVolume.empty())
      Result.missing_volume=OpenCallbackSpec->MissingVolume;
    return Result;
  }

  CExtractCallback *ExtractCallbackSpec=new CExtractCallback(Archive,DestPath,Password);
  CMyComPtr<IArchiveExtractCallback> ExtractCallback=ExtractCallbackSpec;
  HRESULT ExtractResult=Archive->Extract(NULL,(UInt32)(Int32)-1,false,ExtractCallback);
  if (ExtractResult!=S_OK)
  {
    Result.code=ExtractResult==E_OUTOFMEMORY ? RARX_MEMORY:RARX_FATAL;
    Result.reason=ExtractResult==E_OUTOFMEMORY ? "out of memory":"7z extraction error";
    if (VolumeStreamSpec && !VolumeStreamSpec->MissingVolume.empty())
    {
      Result.code=RARX_OPEN;
      Result.reason="missing 7z volume or open error";
      Result.missing_volume=VolumeStreamSpec->MissingVolume;
    }
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
