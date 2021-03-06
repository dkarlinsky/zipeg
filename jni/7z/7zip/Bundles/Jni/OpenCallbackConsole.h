// OpenCallbackConsole.h

#ifndef __OPENCALLBACKCONSOLE_H
#define __OPENCALLBACKCONSOLE_H

#include "Common/StdOutStream.h"
#include "../../UI/Common/ArchiveOpenCallback.h"

class COpenCallbackConsole: public IOpenCallbackUI
{
public:
  HRESULT CheckBreak();
  HRESULT SetTotal(const UInt64 *files, const UInt64 *bytes);
  HRESULT SetCompleted(const UInt64 *files, const UInt64 *bytes);
  HRESULT CryptoGetTextPassword(BSTR *password);
  HRESULT GetPasswordIfAny(UString &password);

  CStdOutStream *OutStream;
  bool PasswordIsDefined;
  UString Password;
  COpenCallbackConsole(): PasswordIsDefined(false) {}
};

#endif
