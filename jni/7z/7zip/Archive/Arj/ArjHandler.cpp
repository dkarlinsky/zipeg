// ArjHandler.cpp

#include "StdAfx.h"

#include "Common/Defs.h"
#include "Common/CRC.h"
#include "Common/StringConvert.h"
#include "Common/ComTry.h"

#include "Windows/Time.h"
#include "Windows/PropVariant.h"

#include "ArjHandler.h"

#include "../../ICoder.h"

#include "../../Common/StreamObjects.h"
#include "../../Common/ProgressUtils.h"
#include "../../Common/LimitedStreams.h"

#include "../../Compress/Copy/CopyCoder.h"
#include "../../Compress/Arj/ArjDecoder1.h"
#include "../../Compress/Arj/ArjDecoder2.h"

#include "../Common/ItemNameUtils.h"
#include "../Common/OutStreamWithCRC.h"

using namespace NWindows;
using namespace NTime;

namespace NArchive {
namespace NArj{

const wchar_t *kHostOS[] = 
{
  L"MSDOS",
  L"PRIMOS",
  L"Unix",
  L"AMIGA",
  L"Mac",
  L"OS/2",
  L"APPLE GS",
  L"Atari ST",
  L"Next",
  L"VAX VMS",
  L"WIN95"
};


const int kNumHostOSes = sizeof(kHostOS) / sizeof(kHostOS[0]);

const wchar_t *kUnknownOS = L"Unknown";


/*
enum // PropID
{
  kpidHostOS = kpidUserDefined,
  kpidUnPackVersion, 
  kpidMethod, 
};
*/

STATPROPSTG kProperties[] = 
{
  { NULL, kpidPath, VT_BSTR},
  { NULL, kpidIsFolder, VT_BOOL},
  { NULL, kpidSize, VT_UI8},
  { NULL, kpidPackedSize, VT_UI8},
  { NULL, kpidLastWriteTime, VT_FILETIME},
  { NULL, kpidAttributes, VT_UI4},

  { NULL, kpidEncrypted, VT_BOOL},
  // { NULL, kpidCommented, VT_BOOL},
    
  { NULL, kpidCRC, VT_UI4},

  { NULL, kpidMethod, VT_UI1},
  { NULL, kpidHostOS, VT_BSTR}

  // { L"UnPack Version", kpidUnPackVersion, VT_UI1},
  // { L"Method", kpidMethod, VT_UI1},
  // { L"Host OS", kpidHostOS, VT_BSTR}
};


CHandler::CHandler()
{}

STDMETHODIMP CHandler::GetArchiveProperty(PROPID /* propID */, PROPVARIANT *value)
{
  value->vt = VT_EMPTY;
  return S_OK;
}

STDMETHODIMP CHandler::GetNumberOfProperties(UInt32 *numProperties)
{
  *numProperties = sizeof(kProperties) / sizeof(kProperties[0]);
  return S_OK;
}

STDMETHODIMP CHandler::GetPropertyInfo(UInt32 index,     
      BSTR *name, PROPID *propID, VARTYPE *varType)
{
  if(index >= sizeof(kProperties) / sizeof(kProperties[0]))
    return E_INVALIDARG;
  const STATPROPSTG &srcItem = kProperties[index];
  *propID = srcItem.propid;
  *varType = srcItem.vt;
  *name = 0;
  return S_OK;
}

STDMETHODIMP CHandler::GetNumberOfArchiveProperties(UInt32 *numProperties)
{
  *numProperties = 0;
  return S_OK;
}

STDMETHODIMP CHandler::GetArchivePropertyInfo(UInt32 /* index */,     
      BSTR * /* name */, PROPID * /* propID */, VARTYPE * /* varType */)
{
  return E_NOTIMPL;
}

STDMETHODIMP CHandler::GetNumberOfItems(UInt32 *numItems)
{
  *numItems = _items.Size();
  return S_OK;
}

STDMETHODIMP CHandler::GetProperty(UInt32 index, PROPID propID,  PROPVARIANT *value)
{
  COM_TRY_BEGIN
  NWindows::NCOM::CPropVariant propVariant;
  const CItemEx &item = _items[index];
  switch(propID)
  {
    case kpidPath:
      propVariant = 
      NItemName::GetOSName(MultiByteToUnicodeString(item.Name, CP_OEMCP));
      /*
                     NItemName::GetOSName2(
          MultiByteToUnicodeString(item.Name, item.GetCodePage()));
          */
      break;
    case kpidIsFolder:
      propVariant = item.IsDirectory();
      break;
    case kpidSize:
      propVariant = item.Size;
      break;
    case kpidPackedSize:
      propVariant = item.PackSize;
      break;
    case kpidLastWriteTime:
    {
      FILETIME localFileTime, utcFileTime;
      if (DosTimeToFileTime(item.ModifiedTime, localFileTime))
      {
        if (!LocalFileTimeToFileTime(&localFileTime, &utcFileTime))
          utcFileTime.dwHighDateTime = utcFileTime.dwLowDateTime = 0;
      }
      else
        utcFileTime.dwHighDateTime = utcFileTime.dwLowDateTime = 0;
      propVariant = utcFileTime;
      break;
    }
    case kpidAttributes:
      propVariant = item.GetWinAttributes();
      break;
    case kpidEncrypted:
      propVariant = item.IsEncrypted();
      break;
    /*
    case kpidCommented:
      propVariant = item.IsCommented();
      break;
    */
    case kpidCRC:
      propVariant = item.FileCRC;
      break;
    case kpidMethod:
      propVariant = item.Method;
      break;
    case kpidHostOS:
      propVariant = (item.HostOS < kNumHostOSes) ?
        (kHostOS[item.HostOS]) : kUnknownOS;
      break;
  }
  propVariant.Detach(value);
  return S_OK;
  COM_TRY_END
}

/*
class CPropgressImp: public CProgressVirt
{
public:
  CMyComPtr<IArchiveOpenCallback> Callback;
  STDMETHOD(SetCompleted)(const UInt64 *numFiles);
};

STDMETHODIMP CPropgressImp::SetCompleted(const UInt64 *numFiles)
{
  if (Callback)
    return Callback->SetCompleted(numFiles, NULL);
  return S_OK;
}
*/

STDMETHODIMP CHandler::Open(IInStream *inStream, 
    const UInt64 *maxCheckStartPosition, IArchiveOpenCallback *callback)
{
  COM_TRY_BEGIN
  try
  {
    _items.Clear();
    CInArchive archive;
    if(!archive.Open(inStream, maxCheckStartPosition))
      return S_FALSE;
    if (callback != NULL)
    {
      RINOK(callback->SetTotal(NULL, NULL));
      UInt64 numFiles = _items.Size();
      RINOK(callback->SetCompleted(&numFiles, NULL));
    }
    for (;;)
    {
      CItemEx itemInfo;
      bool filled;
      HRESULT result = archive.GetNextItem(filled, itemInfo);
      if (result == S_FALSE)
        return S_FALSE;
      if (result != S_OK)
        return S_FALSE;
      if (!filled)
        break;
      _items.Add(itemInfo);
      archive.IncreaseRealPosition(itemInfo.PackSize);
      if (callback != NULL)
      {
        UInt64 numFiles = _items.Size();
        RINOK(callback->SetCompleted(&numFiles, NULL));
      }
    }
    _stream = inStream;
  }
  catch(...)
  {
    return S_FALSE;
  }
  COM_TRY_END
  return S_OK;
}

STDMETHODIMP CHandler::Close()
{
  _items.Clear();
  _stream.Release();
  return S_OK;
}



//////////////////////////////////////
// CHandler::DecompressItems

STDMETHODIMP CHandler::Extract(const UInt32* indices, UInt32 numItems,
    Int32 testModeSpec, IArchiveExtractCallback *extractCallback)
{
  COM_TRY_BEGIN
  bool testMode = (testModeSpec != 0);
  UInt64 totalUnPacked = 0, totalPacked = 0;
  bool allFilesMode = (numItems == UInt32(-1));
  if (allFilesMode)
    numItems = _items.Size();
  if(numItems == 0)
    return S_OK;
  UInt32 i;
  for(i = 0; i < numItems; i++)
  {
    const CItemEx &itemInfo = _items[allFilesMode ? i : indices[i]];
    totalUnPacked += itemInfo.Size;
    totalPacked += itemInfo.PackSize;
  }
  extractCallback->SetTotal(totalUnPacked);

  UInt64 currentTotalUnPacked = 0, currentTotalPacked = 0;
  UInt64 currentItemUnPacked, currentItemPacked;
  
  CMyComPtr<ICompressCoder> arj1Decoder;
  CMyComPtr<ICompressCoder> arj2Decoder;
  CMyComPtr<ICompressCoder> copyCoder;

  for(i = 0; i < numItems; i++, currentTotalUnPacked += currentItemUnPacked,
      currentTotalPacked += currentItemPacked)
  {
    currentItemUnPacked = 0;
    currentItemPacked = 0;

    RINOK(extractCallback->SetCompleted(&currentTotalUnPacked));
    CMyComPtr<ISequentialOutStream> realOutStream;
    Int32 askMode;
    askMode = testMode ? NArchive::NExtract::NAskMode::kTest :
        NArchive::NExtract::NAskMode::kExtract;
    Int32 index = allFilesMode ? i : indices[i];
    const CItemEx &itemInfo = _items[index];
    RINOK(extractCallback->GetStream(index, &realOutStream, askMode));

    if(itemInfo.IsDirectory())
    {
      // if (!testMode)
      {
        RINOK(extractCallback->PrepareOperation(askMode));
        RINOK(extractCallback->SetOperationResult(NArchive::NExtract::NOperationResult::kOK));
      }
      continue;
    }

    if (!testMode && (!realOutStream)) 
      continue;

    RINOK(extractCallback->PrepareOperation(askMode));
    currentItemUnPacked = itemInfo.Size;
    currentItemPacked = itemInfo.PackSize;

    {
      COutStreamWithCRC *outStreamSpec = new COutStreamWithCRC;
      CMyComPtr<ISequentialOutStream> outStream(outStreamSpec);
      outStreamSpec->SetStream(realOutStream);
      outStreamSpec->Init();
      realOutStream.Release();
      
      CLimitedSequentialInStream *streamSpec = new CLimitedSequentialInStream;
      CMyComPtr<ISequentialInStream> inStream(streamSpec);
      
      UInt64 pos;
      _stream->Seek(itemInfo.DataPosition, STREAM_SEEK_SET, &pos);

      streamSpec->SetStream(_stream);
      streamSpec->Init(itemInfo.PackSize);


      CLocalProgress *localProgressSpec = new CLocalProgress;
      CMyComPtr<ICompressProgressInfo> progress = localProgressSpec;
      localProgressSpec->Init(extractCallback, false);


      CLocalCompressProgressInfo *localCompressProgressSpec = 
          new CLocalCompressProgressInfo;
      CMyComPtr<ICompressProgressInfo> compressProgress = localCompressProgressSpec;
      localCompressProgressSpec->Init(progress, 
          &currentTotalPacked,
          &currentTotalUnPacked);

      if (itemInfo.IsEncrypted())
      {
        RINOK(extractCallback->SetOperationResult(
          NArchive::NExtract::NOperationResult::kUnSupportedMethod));
        continue;
      }

      HRESULT result;

      switch(itemInfo.Method)
      {
        case NFileHeader::NCompressionMethod::kStored:
          {
            if(!copyCoder)
              copyCoder = new NCompress::CCopyCoder;
            try
            {
              if (itemInfo.IsEncrypted())
              {
                RINOK(extractCallback->SetOperationResult(
                  NArchive::NExtract::NOperationResult::kUnSupportedMethod));
                continue;
              }
              else
              {
                result = copyCoder->Code(inStream, outStream,
                    NULL, NULL, compressProgress);
              }
              if (result == S_FALSE)
                throw "data error";
              if (result != S_OK)
                return result;
            }
            catch(...)
            {
              outStream.Release();
              RINOK(extractCallback->SetOperationResult(
                  NArchive::NExtract::NOperationResult::kDataError));
              continue;
            }
            break;
          }
        case NFileHeader::NCompressionMethod::kCompressed1a:
        case NFileHeader::NCompressionMethod::kCompressed1b:
        case NFileHeader::NCompressionMethod::kCompressed1c:
          {
            if(!arj1Decoder)
            {
              arj1Decoder = new NCompress::NArj::NDecoder1::CCoder;
            }
            try
            {
              if (itemInfo.IsEncrypted())
              {
                RINOK(extractCallback->SetOperationResult(
                  NArchive::NExtract::NOperationResult::kUnSupportedMethod));
                continue;
              }
              else
              {
                result = arj1Decoder->Code(inStream, outStream,
                    NULL, &currentItemUnPacked, compressProgress);
              }
              if (result == S_FALSE)
                throw "data error";
              if (result != S_OK)
                return result;
            }
            catch(...)
            {
              outStream.Release();
              RINOK(extractCallback->SetOperationResult(
                  NArchive::NExtract::NOperationResult::kDataError));
              continue;
            }
            break;
          }
        case NFileHeader::NCompressionMethod::kCompressed2:
          {
            if(!arj2Decoder)
            {
              arj2Decoder = new NCompress::NArj::NDecoder2::CCoder;
            }
            try
            {
              if (itemInfo.IsEncrypted())
              {
                RINOK(extractCallback->SetOperationResult(
                  NArchive::NExtract::NOperationResult::kUnSupportedMethod));
                continue;
              }
              else
              {
                result = arj2Decoder->Code(inStream, outStream,
                    NULL, &currentItemUnPacked, compressProgress);
              }
              if (result == S_FALSE)
                throw "data error";
              if (result != S_OK)
                return result;
            }
            catch(...)
            {
              outStream.Release();
              RINOK(extractCallback->SetOperationResult(
                  NArchive::NExtract::NOperationResult::kDataError));
              continue;
            }
            break;
          }
        default:
            RINOK(extractCallback->SetOperationResult(
                NArchive::NExtract::NOperationResult::kUnSupportedMethod));
            continue;
      }
      bool crcOK = outStreamSpec->GetCRC() == itemInfo.FileCRC;
      outStream.Release();
      if(crcOK)
        RINOK(extractCallback->SetOperationResult(NArchive::NExtract::NOperationResult::kOK))
      else
        RINOK(extractCallback->SetOperationResult(NArchive::NExtract::NOperationResult::kCRCError))
    }
  }
  return S_OK;
  COM_TRY_END
}


}}
