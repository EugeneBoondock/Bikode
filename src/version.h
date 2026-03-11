/******************************************************************************
*
*
* Notepad2
*
* version.h
*   Notepad2 version information
*
* See Readme.txt for more information about this source code.
* Please send me your comments to this work.
*
* See License.txt for details about distribution and modification.
*
*                                              (c) Florian Balmer 1996-2011
*                                                  florian.balmer@gmail.com
*                                               http://www.flos-freeware.ch
*
*
******************************************************************************/

#include <_version.h>

#define STRINGIFY(x) L#x
#define TOSTRING(x) STRINGIFY(x)

#define VERSION_FILEVERSION_BUILD    0
#define VERSION_FILEVERSION_BUILD_STRING  TOSTRING(VERSION_FILEVERSION_BUILD)
#define VERSION_FILEVERSION_NUM      1,VERSION_FILEVERSION_BUILD
#define VERSION_FILEVERSION_SHORT    L"1." VERSION_FILEVERSION_BUILD_STRING
#define VERSION_LEGALCOPYRIGHT_SHORT L"Copyright � 2004-2011"
#define VERSION_LEGALCOPYRIGHT_LONG  L"� Florian Balmer and contributors"
#define VERSION_COMPANYNAME          L"Boondock Labs"
#define VERSION_PRODUCTNAME          L"Bikode"
#define VERSION_COMPANYURL           L"https://boondocklabs.co.za/"
#define VERSION_PRODUCTURL           L"https://www.bikode.co.za/"
#if defined(ICU_BUILD) && defined(LPEG_LEXER)
#define VERSION_FILEDESCRIPTION_BASE L"Bikode"
#elif ICU_BUILD
#define VERSION_FILEDESCRIPTION_BASE L"Bikode"
#elif LPEG_LEXER
#define VERSION_FILEDESCRIPTION_BASE L"Bikode"
#else
#define VERSION_FILEDESCRIPTION_BASE L"Bikode"
#endif
#ifdef _WIN64
#define VERSION_FILEDESCRIPTION      VERSION_FILEDESCRIPTION_BASE L" x64"
#else
#define VERSION_FILEDESCRIPTION      VERSION_FILEDESCRIPTION_BASE
#endif
#if defined(ICU_BUILD) && defined(LPEG_LEXER)
#define VERSION_INTERNALNAME         L"Bikode"
#define VERSION_ORIGINALFILENAME     L"Bikode.exe"
#elif ICU_BUILD
#define VERSION_INTERNALNAME         L"Bikode"
#define VERSION_ORIGINALFILENAME     L"Bikode.exe"
#elif LPEG_LEXER
#define VERSION_INTERNALNAME         L"Bikode"
#define VERSION_ORIGINALFILENAME     L"Bikode.exe"
#else
#define VERSION_INTERNALNAME         L"Bikode"
#define VERSION_ORIGINALFILENAME     L"Bikode.exe"
#endif
#define VERSION_AUTHORNAME           VERSION_COMPANYNAME
#define VERSION_WEBPAGEDISPLAY       L"Boondock Labs - https://boondocklabs.co.za/"
#define VERSION_EMAILDISPLAY         L"florian.balmer@gmail.com"
#define VERSION_EXT_VERSION          L"Extended Edition � 2013-"BUILD_YEAR_STR
#define VERSION_EXT_BY               L"Built by Boondock Labs - https://boondocklabs.co.za/"
#define VERSION_EXT_PAGE             VERSION_PRODUCTURL
#define VERSION_COMMIT               L"6c3f5ac"
