// ArchiveExtractCallback.cpp

#include "StdAfx.h"

#include "ArchiveExtractCallback.h"

#include "Common/Wildcard.h"
#include "Common/StringConvert.h"
#include "Common/ComTry.h"

#include "Windows/FileDir.h"
#include "Windows/FileFind.h"
#include "Windows/Time.h"
#include "Windows/Defs.h"
#include "Windows/PropVariant.h"

#include "Windows/PropVariantConversions.h"

#include "../../Common/FilePathAutoRename.h"

#include "../Common/ExtractingFilePath.h"
#include "OpenArchive.h"

using namespace NWindows;

static const wchar_t *kCantAutoRename = L"ERROR: Can not create file with auto name";
static const wchar_t *kCantRenameFile = L"ERROR: Can not rename existing file ";
static const wchar_t *kCantDeleteOutputFile = L"ERROR: Can not delete output file ";


void CArchiveExtractCallback::Init(
    IInArchive *archiveHandler,
    IFolderArchiveExtractCallback *extractCallback2,
    bool stdOutMode,
    const UString &directoryPath, 
    NExtract::NPathMode::EEnum pathMode,
    NExtract::NOverwriteMode::EEnum overwriteMode,
    const UStringVector &removePathParts,
    const UString &itemDefaultName,
    const FILETIME &utcLastWriteTimeDefault,
    UInt32 attributesDefault)
{
  _stdOutMode = stdOutMode;
  _numErrors = 0;
  _extractCallback2 = extractCallback2;
  _itemDefaultName = itemDefaultName;
  _utcLastWriteTimeDefault = utcLastWriteTimeDefault;
  _attributesDefault = attributesDefault;
  _removePathParts = removePathParts;
  _pathMode = pathMode;
  _overwriteMode = overwriteMode;
  _archiveHandler = archiveHandler;
  _directoryPath = directoryPath;
  NFile::NName::NormalizeDirPathPrefix(_directoryPath);
}

STDMETHODIMP CArchiveExtractCallback::SetTotal(UInt64 size)
{
  COM_TRY_BEGIN
  return _extractCallback2->SetTotal(size);
  COM_TRY_END
}

STDMETHODIMP CArchiveExtractCallback::SetCompleted(const UInt64 *completeValue)
{
  COM_TRY_BEGIN
  return _extractCallback2->SetCompleted(completeValue);
  COM_TRY_END
}

void CArchiveExtractCallback::CreateComplexDirectory(const UStringVector &dirPathParts, UString &fullPath)
{
  fullPath = _directoryPath;
  for(int i = 0; i < dirPathParts.Size(); i++)
  {
    if (i > 0)
      fullPath += wchar_t(NFile::NName::kDirDelimiter);
    fullPath += dirPathParts[i];
    NFile::NDirectory::MyCreateDirectory(fullPath);
  }
}

static UString MakePathNameFromParts(const UStringVector &parts)
{
  UString result;
  for(int i = 0; i < parts.Size(); i++)
  {
    if(i != 0)
      result += wchar_t(NFile::NName::kDirDelimiter);
    result += parts[i];
  }
  return result;
}


HRESULT CArchiveExtractCallback::GetTime(int index, PROPID propID, FILETIME &filetime, bool &filetimeIsDefined)
{
  filetimeIsDefined = false;
  NCOM::CPropVariant prop;
  RINOK(_archiveHandler->GetProperty(index, propID, &prop));
  if (prop.vt == VT_FILETIME)
  {
    filetime = prop.filetime;
    filetimeIsDefined = true;
  }
  else if (prop.vt != VT_EMPTY)
    return E_FAIL;
  return S_OK;
}

STDMETHODIMP CArchiveExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode)
{
  COM_TRY_BEGIN
  *outStream = 0;
  _outFileStream.Release();

  _encrypted = false;
  _isSplit = false;

  UString fullPath;
  {
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidPath, &prop));
    
    if(prop.vt == VT_EMPTY)
      fullPath = _itemDefaultName;
    else 
    {
      if(prop.vt != VT_BSTR)
        return E_FAIL;
      fullPath = prop.bstrVal;
    }
  }

  // UString fullPathCorrect = GetCorrectPath(fullPath);
  _filePath = fullPath;

  {
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidPosition, &prop));
    if (prop.vt != VT_EMPTY)
    {
      if (prop.vt != VT_UI8)
        return E_FAIL;
      _position = prop.uhVal.QuadPart;
      _isSplit = true;
    }
  }
    
  {
    NCOM::CPropVariant prop;
    RINOK(_archiveHandler->GetProperty(index, kpidEncrypted, &prop));
    if (prop.vt == VT_BOOL)
      _encrypted = VARIANT_BOOLToBool(prop.boolVal);
    else if (prop.vt != VT_EMPTY)
      return E_FAIL;
  }

  if(askExtractMode == NArchive::NExtract::NAskMode::kExtract)
  {
    if (_stdOutMode)
    {
      CMyComPtr<ISequentialOutStream> outStreamLoc = new CStdOutFileStream;
      *outStream = outStreamLoc.Detach();
      return S_OK;
    }

    {
      NCOM::CPropVariant prop;
      RINOK(_archiveHandler->GetProperty(index, kpidAttributes, &prop));
      if (prop.vt == VT_EMPTY)
      {
        _processedFileInfo.Attributes = _attributesDefault;
        _processedFileInfo.AttributesAreDefined = false;
      }
      else
      {
        if (prop.vt != VT_UI4)
          throw "incorrect item";
        _processedFileInfo.Attributes = prop.ulVal;
        _processedFileInfo.AttributesAreDefined = true;
      }
    }

    RINOK(IsArchiveItemFolder(_archiveHandler, index, _processedFileInfo.IsDirectory));

    RINOK(GetTime(index, kpidCreationTime, _processedFileInfo.CreationTime,
        _processedFileInfo.IsCreationTimeDefined));
    RINOK(GetTime(index, kpidLastWriteTime, _processedFileInfo.LastWriteTime, 
        _processedFileInfo.IsLastWriteTimeDefined));
    RINOK(GetTime(index, kpidLastAccessTime, _processedFileInfo.LastAccessTime,
        _processedFileInfo.IsLastAccessTimeDefined));

    bool newFileSizeDefined;
    UInt64 newFileSize;
    {
      NCOM::CPropVariant prop;
      RINOK(_archiveHandler->GetProperty(index, kpidSize, &prop));
      newFileSizeDefined = (prop.vt != VT_EMPTY);
      if (newFileSizeDefined)
        newFileSize = ConvertPropVariantToUInt64(prop);
    }

    bool isAnti = false;
    {
      NCOM::CPropVariant prop;
      RINOK(_archiveHandler->GetProperty(index, kpidIsAnti, &prop));
      if (prop.vt == VT_BOOL)
        isAnti = VARIANT_BOOLToBool(prop.boolVal);
    }

    UStringVector pathParts; 
    SplitPathToParts(fullPath, pathParts);
    
    if(pathParts.IsEmpty())
      return E_FAIL;
    UString processedPath;
    switch(_pathMode)
    {
      case NExtract::NPathMode::kFullPathnames:
      {
        processedPath = fullPath;
        break;
      }
      case NExtract::NPathMode::kCurrentPathnames:
      {
        // for incorrect paths: "/dir1/dir2/file"
        int numRemovePathParts = _removePathParts.Size();
        if(pathParts.Size() <= numRemovePathParts)
          return E_FAIL;
        for(int i = 0; i < numRemovePathParts; i++)
          if(_removePathParts[i].CompareNoCase(pathParts[i]) != 0)
            return E_FAIL;
        pathParts.Delete(0, numRemovePathParts);
        processedPath = MakePathNameFromParts(pathParts);
        break;
      }
      case NExtract::NPathMode::kNoPathnames:
      {
        processedPath = pathParts.Back(); 
        pathParts.Delete(0, pathParts.Size() - 1); // Test it!!
        break;
      }
    }
    processedPath = GetCorrectPath(processedPath);
    if(!_processedFileInfo.IsDirectory)
      pathParts.DeleteBack();

    MakeCorrectPath(pathParts);
    
    if (!isAnti)
    {
      if (!pathParts.IsEmpty())
      {
        UString fullPathNew;
        CreateComplexDirectory(pathParts, fullPathNew);
        if (_processedFileInfo.IsDirectory)
          NFile::NDirectory::SetDirTime(fullPathNew, 
            (WriteCreated && _processedFileInfo.IsCreationTimeDefined) ? &_processedFileInfo.CreationTime : NULL, 
            (WriteAccessed && _processedFileInfo.IsLastAccessTimeDefined) ? &_processedFileInfo.LastAccessTime : NULL, 
            (WriteModified && _processedFileInfo.IsLastWriteTimeDefined) ? &_processedFileInfo.LastWriteTime : &_utcLastWriteTimeDefault);
      }
    }


    UString fullProcessedPath = _directoryPath + processedPath;

    if(_processedFileInfo.IsDirectory)
    {
      _diskFilePath = fullProcessedPath;
      if (isAnti)
        NFile::NDirectory::MyRemoveDirectory(_diskFilePath);
      return S_OK;
    }

    if (!_isSplit)
    {
    NFile::NFind::CFileInfoW fileInfo;
    if(NFile::NFind::FindFile(fullProcessedPath, fileInfo))
    {
      switch(_overwriteMode)
      {
        case NExtract::NOverwriteMode::kSkipExisting:
          return S_OK;
        case NExtract::NOverwriteMode::kAskBefore:
        {
          Int32 overwiteResult;
		  FILETIME fwt;
		  FILETIME pwt;
		  UINT64 size;
          RINOK(_extractCallback2->AskOverwrite(
              fullProcessedPath, &fwt, &size, fullPath, 
              _processedFileInfo.IsLastWriteTimeDefined ? &pwt : NULL, 
              newFileSizeDefined ? &newFileSize : NULL, 
              &overwiteResult))
		  fileInfo.setLastWriteTime(fwt);
		  fileInfo.setSize(size);
		  if (_processedFileInfo.IsLastWriteTimeDefined)
			_processedFileInfo.LastWriteTime = pwt;
          switch(overwiteResult)
          {
            case NOverwriteAnswer::kCancel:
              return E_ABORT;
            case NOverwriteAnswer::kNo:
              return S_OK;
            case NOverwriteAnswer::kNoToAll:
              _overwriteMode = NExtract::NOverwriteMode::kSkipExisting;
              return S_OK;
            case NOverwriteAnswer::kYesToAll:
              _overwriteMode = NExtract::NOverwriteMode::kWithoutPrompt;
              break;
            case NOverwriteAnswer::kYes:
              break;
            case NOverwriteAnswer::kAutoRename:
              _overwriteMode = NExtract::NOverwriteMode::kAutoRename;
              break;
            default:
              throw 20413;
          }
        }
      }
      if (_overwriteMode == NExtract::NOverwriteMode::kAutoRename)
      {
        if (!AutoRenamePath(fullProcessedPath))
        {
          UString message = UString(kCantAutoRename) + 
              fullProcessedPath;
          RINOK(_extractCallback2->MessageError(message));
          return E_ABORT;
        }
      }
      else if (_overwriteMode == NExtract::NOverwriteMode::kAutoRenameExisting)
      {
        UString existPath = fullProcessedPath;
        if (!AutoRenamePath(existPath))
        {
          UString message = kCantAutoRename + fullProcessedPath;
          RINOK(_extractCallback2->MessageError(message));
          return E_ABORT;
        }
        if(!NFile::NDirectory::MyMoveFile(fullProcessedPath, existPath))
        {
          UString message = UString(kCantRenameFile) + fullProcessedPath;
          RINOK(_extractCallback2->MessageError(message));
          return E_ABORT;
        }
      }
      else
        if (!NFile::NDirectory::DeleteFileAlways(fullProcessedPath))
        {
          UString message = UString(kCantDeleteOutputFile) + 
              fullProcessedPath;
          RINOK(_extractCallback2->MessageError(message));
          return E_ABORT;
        }
    }
    }
    if (!isAnti)
    {
      _outFileStreamSpec = new COutFileStream;
      CMyComPtr<ISequentialOutStream> outStreamLoc(_outFileStreamSpec);
      if (!_outFileStreamSpec->File.Open(fullProcessedPath, 
          _isSplit ? OPEN_ALWAYS: CREATE_ALWAYS))
      {
        // if (::GetLastError() != ERROR_FILE_EXISTS || !isSplit)
        {
          UString message = L"can not open output file " + fullProcessedPath;
          RINOK(_extractCallback2->MessageError(message));
          return S_OK;
        }
      }
      if (_isSplit)
      {
        RINOK(_outFileStreamSpec->Seek(_position, STREAM_SEEK_SET, NULL));
      }
      _outFileStream = outStreamLoc;
      *outStream = outStreamLoc.Detach();
    }
    _diskFilePath = fullProcessedPath;
  }
  else
  {
    *outStream = NULL;
  }
  return S_OK;
  COM_TRY_END
}

STDMETHODIMP CArchiveExtractCallback::PrepareOperation(Int32 askExtractMode)
{
  COM_TRY_BEGIN
  _extractMode = false;
  switch (askExtractMode)
  {
    case NArchive::NExtract::NAskMode::kExtract:
      _extractMode = true;
  };
  return _extractCallback2->PrepareOperation(_filePath, askExtractMode, _isSplit ? &_position: 0);
  COM_TRY_END
}

STDMETHODIMP CArchiveExtractCallback::SetOperationResult(Int32 operationResult)
{
  COM_TRY_BEGIN
  switch(operationResult)
  {
    case NArchive::NExtract::NOperationResult::kOK:
    case NArchive::NExtract::NOperationResult::kUnSupportedMethod:
    case NArchive::NExtract::NOperationResult::kCRCError:
    case NArchive::NExtract::NOperationResult::kDataError:
      break;
    default:
      _outFileStream.Release();
      return E_FAIL;
  }
  if(_outFileStream != NULL)
    _outFileStreamSpec->File.SetTime(
        (WriteCreated && _processedFileInfo.IsCreationTimeDefined) ? &_processedFileInfo.CreationTime : NULL, 
        (WriteAccessed && _processedFileInfo.IsLastAccessTimeDefined) ? &_processedFileInfo.LastAccessTime : NULL, 
        (WriteModified && _processedFileInfo.IsLastWriteTimeDefined) ? &_processedFileInfo.LastWriteTime : &_utcLastWriteTimeDefault);
  _outFileStream.Release();
  if (_extractMode && _processedFileInfo.AttributesAreDefined)
    NFile::NDirectory::MySetFileAttributes(_diskFilePath, _processedFileInfo.Attributes);
  RINOK(_extractCallback2->SetOperationResult(operationResult, _encrypted));
  return S_OK;
  COM_TRY_END
}

/*
STDMETHODIMP CArchiveExtractCallback::GetInStream(
    const wchar_t *name, ISequentialInStream **inStream)
{
  COM_TRY_BEGIN
  CInFileStream *inFile = new CInFileStream;
  CMyComPtr<ISequentialInStream> inStreamTemp = inFile;
  if (!inFile->Open(_srcDirectoryPrefix + name))
    return ::GetLastError();
  *inStream = inStreamTemp.Detach();
  return S_OK;
  COM_TRY_END
}
*/

STDMETHODIMP CArchiveExtractCallback::CryptoGetTextPassword(BSTR *password)
{
  COM_TRY_BEGIN
  if (!_cryptoGetTextPassword)
  {
    RINOK(_extractCallback2.QueryInterface(IID_ICryptoGetTextPassword, 
        &_cryptoGetTextPassword));
  }
  return _cryptoGetTextPassword->CryptoGetTextPassword(password);
  COM_TRY_END
}

