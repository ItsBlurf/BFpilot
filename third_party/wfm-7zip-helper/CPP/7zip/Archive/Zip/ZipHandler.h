// Zip/Handler.h

#ifndef ZIP7_INC_ZIP_HANDLER_H
#define ZIP7_INC_ZIP_HANDLER_H

#include "../../../Common/DynamicBuffer.h"
#include "../../ICoder.h"
#include "../IArchive.h"

#include "../../Common/CreateCoder.h"

#ifdef Z7_EXTRACT_ONLY
#include "../Common/HandlerOut.h"
#else
#include "ZipCompressionMode.h"
#endif
#include "ZipIn.h"

namespace NArchive {
namespace NZip {

#ifdef Z7_EXTRACT_ONLY
const CMethodId kMethodId_ZipBase = 0x040100;
const CMethodId kMethodId_BZip2   = 0x040202;
#endif

const unsigned kNumMethodNames1 = NFileHeader::NCompressionMethod::kZstdPk + 1;
const unsigned kMethodNames2Start = NFileHeader::NCompressionMethod::kZstdWz;
const unsigned kNumMethodNames2 = NFileHeader::NCompressionMethod::kWzAES + 1 - kMethodNames2Start;

extern const char * const kMethodNames1[kNumMethodNames1];
extern const char * const kMethodNames2[kNumMethodNames2];


class CHandler Z7_final:
  public IInArchive,
  // public IArchiveGetRawProps,
#ifndef Z7_EXTRACT_ONLY
  public IOutArchive,
  public ISetProperties,
#endif
  Z7_PUBLIC_ISetCompressCodecsInfo_IFEC
  public CMyUnknownImp
{
  Z7_COM_QI_BEGIN2(IInArchive)
  // Z7_COM_QI_ENTRY(IArchiveGetRawProps)
#ifndef Z7_EXTRACT_ONLY
  Z7_COM_QI_ENTRY(IOutArchive)
  Z7_COM_QI_ENTRY(ISetProperties)
#endif
  Z7_COM_QI_ENTRY_ISetCompressCodecsInfo_IFEC
  Z7_COM_QI_END
  Z7_COM_ADDREF_RELEASE

  Z7_IFACE_COM7_IMP(IInArchive)
  // Z7_IFACE_COM7_IMP(IArchiveGetRawProps)
#ifndef Z7_EXTRACT_ONLY
  Z7_IFACE_COM7_IMP(IOutArchive)
  Z7_IFACE_COM7_IMP(ISetProperties)
#endif
  DECL_ISetCompressCodecsInfo

private:
  CObjectVector<CItemEx> m_Items;
  CInArchive m_Archive;

#ifdef Z7_EXTRACT_ONLY
  CCommonMethodProps _props;
#else
  CBaseProps _props;
  CHandlerTimeOptions TimeOptions;
  int m_MainMethod;
  bool m_ForceAesMode;
  bool _removeSfxBlock;
#endif
  bool m_ForceLocal;
  bool m_ForceUtf8;
  bool _force_SeqOutMode; // for creation
  bool _force_OpenSeq;
  bool _forceCodePage;
  UInt32 _specifiedCodePage;

  DECL_EXTERNAL_CODECS_VARS

  void InitMethodProps()
  {
#ifndef Z7_EXTRACT_ONLY
    _props.Init();
    TimeOptions.Init();
    TimeOptions.Prec = k_PropVar_TimePrec_0;
    m_MainMethod = -1;
    m_ForceAesMode = false;
    _removeSfxBlock = false;
#endif
    m_ForceLocal = false;
    m_ForceUtf8 = false;
    _force_SeqOutMode = false;
    _force_OpenSeq = false;
    _forceCodePage = false;
    _specifiedCodePage = CP_OEMCP;
  }

  // void MarkAltStreams(CObjectVector<CItemEx> &items);

#ifndef Z7_EXTRACT_ONLY
  HRESULT GetOutProperty(IArchiveUpdateCallback *callback, UInt32 callbackIndex, Int32 arcIndex, PROPID propID, PROPVARIANT *value);
#endif

public:
  CHandler();
};

}}

#endif
