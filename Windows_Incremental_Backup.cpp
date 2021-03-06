// Windows_Incremental_Backup.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 1
#include <iostream>
#include <cstring>
// Backup.cpp : Defines the entry point for the application.
//

#include <stdio.h>
#include <windows.h>

/* TODO:
Change log format to H:M:S.ms
Print log open time-version in log
Create log file name with H:M:S.ms
Add support for FormatString to format run time errors (like in PPCIP)
Print total bytes copied, total # of files copied, # pruned, # dirs created/pruned
GUI

Make this a service.  We can't rely on scheduled tasks because the user password might change.

*/

// Windows documentation states that Windows allows for a max path of approximately 32000 unicode chars
// (which would be 64k since each unicode char is 2 bytes).  So let's double that
#define MAXDIRFILENAME 131072 // 128k

FILE *fLogFile = 0;

DWORD  nDirCount = 0;
DWORD  nFileCount = 0;

BOOL bVerbose = 0;

char szDebug[8192];
wchar_t* pOutputBuffer;

PUCHAR pCopyBuffer;

BOOL gbPrune = 0;

wchar_t szSourceRoot[4096];
wchar_t szDestRoot[4096];
wchar_t szExcludeDirs[16][8192] = { 0 };

void _ods(const wchar_t *pOutputBuffer)
{
	OutputDebugString(pOutputBuffer);

	if (fLogFile)
	{
		SYSTEMTIME sysTime;

		// print date
		GetLocalTime(&sysTime);

		fwprintf(fLogFile, L"[%2.2d:%2.2d:%2.2d.%3.3u] ", sysTime.wHour, sysTime.wMinute, sysTime.wSecond, sysTime.wMilliseconds);
		//fwrite(pOutputBuffer, sizeof(WCHAR), lstrlenW(pOutputBuffer), fLogFile);
		fputws(pOutputBuffer, fLogFile);
		fflush(fLogFile);
	}
}

void ReportSystemError(const wchar_t* lpszErrmsg, DWORD dwError)
{
	LPVOID lpMsgBuf;
	LPVOID lpDisplayBuf;

	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dwError,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);

	lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT,
		(lstrlen((LPTSTR)lpMsgBuf) + lstrlenW(lpszErrmsg) + 40) * sizeof(wchar_t*));
	//memset(lpDisplayBuf, '\0', lstrlen((LPTSTR)lpMsgBuf) + lstrlenW(lpszErrmsg) + 40) * sizeof(TCHAR));
	wsprintf((LPTSTR)lpDisplayBuf,
		L"%s failed with error %d: %s", lpszErrmsg, dwError, lpMsgBuf);

	//MessageBox(0, (LPTSTR)lpDisplayBuf, "Backup", MB_ICONHAND);
	// Print the message
	_ods((const wchar_t*)lpDisplayBuf);

	LocalFree(lpMsgBuf);
	LocalFree(lpDisplayBuf);
}

// print and exit
void FATALODS(const wchar_t* lpOutputString, ...)
{
	va_list args;

	va_start(args, lpOutputString);
	wvsprintf(pOutputBuffer, lpOutputString, args);
	va_end(args);

	_ods(pOutputBuffer);

	OutputDebugString(L"\n\n");
	ReportSystemError((LPTSTR)"FATALODS, calling function", GetLastError());
	OutputDebugString(L"\n\n");
	MessageBox(0, pOutputBuffer, L"Backup", MB_ICONHAND);
	exit(0);
}

// nicer version of OutputDebugString
void ODS(const wchar_t* lpOutputString, ...)
{
	va_list args;

	va_start(args, lpOutputString);
	wvsprintf(pOutputBuffer, lpOutputString, args);
	va_end(args);

	_ods(pOutputBuffer);
}

// if bWrite is true we will not try the alternate filename
FILE *GetFileHandle(WIN32_FIND_DATA *pWfdStruct, BOOL bWrite)
{
	FILE *fFile;
	DWORD nGLE;

	fFile = _wfopen(pWfdStruct->cFileName, L"rb");
	if (!fFile)
	{
		nGLE = GetLastError();

		if (nGLE == ERROR_SHARING_VIOLATION)    // 32
		{
			wchar_t szCurDir[8192];

			GetCurrentDirectory(8192, szCurDir);
			ODS((LPTSTR)"WARN: cannot open %s file due to sharing violation [fRenameFile: %s\\%s]\n",
				bWrite ? "dst" : "src", szCurDir, pWfdStruct->cFileName);
			return 0;
		}

		if (nGLE == ERROR_ACCESS_DENIED)        // 5
		{
			wchar_t szCurDir[8192];

			GetCurrentDirectory(8192, szCurDir);
			ODS(L"WARN: cannot open %s file, access denied [fRenameFile: %s\\%s]\n",
				bWrite ? "dst" : "src", szCurDir, pWfdStruct->cFileName);
			return 0;
		}

		if (nGLE == ERROR_FILE_NOT_FOUND)       // 2
		{
			wchar_t szCurDir[8192];

			GetCurrentDirectory(8192, szCurDir);
			ODS(L"WARN: cannot open %s file, file not present [fRenameFile: %s\\%s]\n",
				bWrite ? "dst" : "src", szCurDir, pWfdStruct->cFileName);
			return 0;
		}

		wchar_t szCurDir[8192];
		GetCurrentDirectory(8192, szCurDir);

		if (bWrite)
		{
			ODS(L"WARN: cannot open dst file [fRenameFile: %s\\%s] [GLE: %d]\n",
				szCurDir, pWfdStruct->cFileName, nGLE);
			return 0;
		}

		ODS((LPTSTR)"WARN: cannot open src file [fRenameFile: %s\\%s] [GLE: %d] trying alt name [%s]\n",
			szCurDir, pWfdStruct->cFileName, nGLE, pWfdStruct->cAlternateFileName);
		ReportSystemError((LPTSTR)"GetFileHandle", nGLE);

		__try
		{
			if (pWfdStruct->cAlternateFileName[0])
			{
				fFile = _wfopen(pWfdStruct->cAlternateFileName, L"rb");
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ODS(L"Exception caught\n");  // give up
		}

		if (!fFile)
		{
			nGLE = GetLastError();
			ReportSystemError((LPTSTR)"GetFileHandle 2nd try", nGLE);
			FATALODS((LPTSTR)"FATAL error, cannot open src file [%s] [GLE: %d]", pWfdStruct->cFileName, nGLE);
		}
		else
		{
			ODS(L"File: %s open success\n", pWfdStruct->cAlternateFileName);
		}
	}

	return fFile;
} // GetFileHandle

BOOL SetAndCheckDir(wchar_t *pDirName, BOOL bExitUponFail)
{
	BOOL bRetVal;
	int nLen;

	bRetVal = SetCurrentDirectory(pDirName);
	if (!bRetVal)
	{
		ReportSystemError((LPTSTR)"SetAndCheckDir", GetLastError());
		if (bExitUponFail)
		{
			FATALODS((LPTSTR)"SetAndCheckDir: FATAL error, Invalid directory [%s]", pDirName);
		}
		else
		{
			ODS(L"SetAndCheckDir: ERROR, Invalid directory [%s]\n", pDirName);
			return FALSE;
		}
	}

	nLen = lstrlenW(pDirName);
	if (pDirName[nLen - 1] != '\\')
	{
		pDirName[nLen] = '\\';
		pDirName[nLen + 1] = 0;
	}

	return TRUE;
}

BOOL CheckForExcludes(wchar_t *pDirName)
{
	int i;

	for (i = 0; i < 16; i++)
	{
		if ((szExcludeDirs[i] != 0) && !lstrcmpW(pDirName, szExcludeDirs[i]))
		{
			ODS(L"CheckForExcludes: excluding %s\n", pDirName);
			return TRUE;
		}
	}

	return FALSE;
}

// NOTE: these calls:
// SetCurrentDirectory()
// FindFirstFile("*.*", &wfdStruct)
// each create a outstanding dir handle

void SourceDestScan(wchar_t *pszCurSource, wchar_t *pszCurDest)
{
	BOOL  bFindRetval = true;
	DWORD nFileSize;
	FILE *fSrcFile;
	FILE *fDstFile;
	BOOL bRetVal;
	wchar_t *pszNextSrc = NULL;
	wchar_t *pszNextDst = NULL;
	DWORD nGLE;
	ULONG nRet;
	BOOL bPerformCopyPrune = 0;

	HANDLE hFindHandle, hDstFindHandle;
	WIN32_FIND_DATA wfdStruct, wfdStructDest;

	hDstFindHandle = fSrcFile = fDstFile = 0;
	hFindHandle = FindFirstFile(L"*.*", &wfdStruct);     // creates a outstanding dir handle

	while (1)
	{
		if (bFindRetval == false)
		{
			nRet = GetLastError();
			if (nRet != ERROR_NO_MORE_FILES)
			{
				ReportSystemError((LPTSTR)"SourceDestScan", nRet);
				FATALODS(L"unkown ERROR, GetLastError ret: %d", nRet);
			}

			if (hDstFindHandle) FindClose(hDstFindHandle);
			if (hFindHandle) FindClose(hFindHandle);

			return;
		}

		//if cFileName is '.' or '..' get the next file
		if (!lstrcmpW(wfdStruct.cFileName, L".") ||
			!lstrcmpW(wfdStruct.cFileName, L".."))
		{
			goto getnextfile;
		}

		/* DEBUG
		if (!strcmp(wfdStruct.cFileName, "My Music"))
		{
			char szTesto[8192];

			GetCurrentDirectory(8192, szTesto);
			printf("here\n");
		}
		*/

		if (wfdStruct.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			nDirCount++;
			if (bVerbose)
			{
				ODS(L"Dir : %s%s\n", pszCurSource, wfdStruct.cFileName);
			}

			// make sure we don't look in the System Volume Information dir
			if (!lstrcmpW(wfdStruct.cFileName, L"System Volume Information"))
			{
				goto getnextfile;
			}

			// does the dest dir exist?, if not try to create it (+2 for \)
			size_t pszCurSourceLen = lstrlenW(pszCurSource) + lstrlenW(wfdStruct.cFileName) + 2;
			pszNextSrc = (wchar_t*)malloc(sizeof(wchar_t) * pszCurSourceLen);

			size_t pszCurDestLen = lstrlenW(pszCurDest) + lstrlenW(wfdStruct.cFileName) + 2;
			pszNextDst = (wchar_t*)malloc(sizeof(wchar_t) * pszCurDestLen);
			
			if (!pszNextSrc || !pszNextDst)
			{
				ReportSystemError((LPTSTR)"SourceDestScan", GetLastError());
				FATALODS(L"SourceScan: malloc failure");
			}
			memset(pszNextSrc, '\0', lstrlenW(pszCurSource) + lstrlenW(wfdStruct.cFileName) + 2);
			memset(pszNextDst, '\0', lstrlenW(pszCurDest) + lstrlenW(wfdStruct.cFileName) + 2);

			wsprintf((wchar_t *)pszNextSrc, L"%s%s\\", pszCurSource, wfdStruct.cFileName);
			wsprintf((wchar_t *)pszNextDst, L"%s%s\\", pszCurDest, wfdStruct.cFileName);

			// Do we want' to exclude this directory?
			if (CheckForExcludes(pszNextSrc))
			{
				goto getnextfile;
			}

			// set the dest dir
			//wchar_t * temp = (wchar_t*)malloc(lstrlenW(pszNextDst));
			//lstrcpyW(temp, pszNextDst);

			bRetVal = SetCurrentDirectory(pszNextDst);
			if (!bRetVal)
			{
				nGLE = GetLastError();

				// for access denied, skip this.  For everything else, report an error and attempt to back up
				if (nGLE == ERROR_ACCESS_DENIED)
				{
					ODS(L"Can't set next dest dir [%s] - ERROR_ACCESS_DENIED\n", pszNextDst);
					goto getnextfile;
				}
				if ((nGLE != ERROR_FILE_NOT_FOUND) && (nGLE != ERROR_PATH_NOT_FOUND))
				{
					ReportSystemError((LPTSTR)"SourceDestScan", nGLE);
				}

				if (gbPrune)
				{
					ODS(L"Pruning %s\n", pszNextSrc);
					bPerformCopyPrune = 1;
				}
				else
				{
					ODS(L"Backing up %s to %s\n", pszNextSrc, pszNextDst);

					if (szDestRoot[0] != pszNextDst[0])
					{
						FATALODS(L"ERROR, dir name mismatch! [%s vs %s]\n", szDestRoot, pszNextSrc);
					}

					bRetVal = CreateDirectory((LPTSTR)pszNextDst, NULL);
					if (!bRetVal)
					{
						FATALODS(L"CreateDirectory failed [%s] [GLE: %d]", pszNextDst, GetLastError());
					}
				}
			}

			// set and proceed into and out of the source dir
			if (!SetAndCheckDir(pszNextSrc, FALSE))         // creates a outstanding dir handle
			{
				goto getnextfile;   // just go to the next file if we get here
			}
			SourceDestScan(pszNextSrc, pszNextDst);
			if (!SetAndCheckDir(pszCurSource, FALSE))
			{
				goto getnextfile;   // just go to the next file if we get here
			}

			if (gbPrune && bPerformCopyPrune)
			{
				if (szDestRoot[0] != pszNextSrc[0])
				{
					FATALODS(L"ERROR, dir name mismatch! [%s vs %s]\n", szDestRoot, pszNextSrc);
				}

				bRetVal = RemoveDirectory(pszNextSrc);
				if (!bRetVal)
				{
					ReportSystemError((LPTSTR)"SourceDestScan: RemoveDirectory", nGLE);
					ODS(L"RemoveDirectory failed [%s]\n", pszNextDst);
				}

				bPerformCopyPrune = 0;
			}

			goto getnextfile;
		}

		// This is a file
		if (bVerbose)
		{
			ODS(L"File: %s%s\n", pszCurSource, wfdStruct.cFileName);
		}

		nFileCount++;

		fSrcFile = GetFileHandle(&wfdStruct, false);    // creates a outstanding dir handle (after delete)
		if (!fSrcFile)
		{
			goto getnextfile;   // there was a minor problem, probably sharing issue
		}

		// dest file?
		bRetVal = SetCurrentDirectory(pszCurDest);
		if (!bRetVal)
		{
			nGLE = GetLastError();
			if (gbPrune)
			{
				bPerformCopyPrune = 1;
				hDstFindHandle = INVALID_HANDLE_VALUE;
			}
			else
			{
				ReportSystemError((LPTSTR)"SourceDestScan", nGLE);
				FATALODS(L"FATAL error, Can't set cur dest dir [%s]", pszCurDest);
			}
		}

		hDstFindHandle = FindFirstFile(wfdStruct.cFileName, &wfdStructDest);    // creates a outstanding dir handle

		if (hDstFindHandle != INVALID_HANDLE_VALUE)
		{
			// if pruning, don't delete if sizes are different (in case we later fail to copy the newer file)
#if 0
	// need to work on this.  In the case we have a file and a dir both with same name, this may cause trouble
			if (gbPrune)
			{
				if (wfdStructDest.dwFileAttributes != wfdStruct.dwFileAttributes)
				{
					// do delete if attributes changed.  We probably got here
					bPerformCopyPrune = 1;
				}
			}
			else
#else
			if (!gbPrune)
#endif
			{
				if (wfdStruct.nFileSizeHigh)
				{
					ODS(L"Backup gives up on files larger than 4gb [high: %d] [low: %d] [file: %s%s]",
						wfdStruct.nFileSizeHigh, wfdStructDest.nFileSizeLow, pszCurSource, wfdStruct.cFileName);
					goto getnextfile;
				}

				// get the source file's actual file size
				fseek(fSrcFile, 0, SEEK_END);
				nFileSize = ftell(fSrcFile);
				if (nFileSize <= 0 && bVerbose)
				{
					ODS(L"INFO: %s%s size is 0\n", pszCurSource, wfdStruct.cFileName);
				}

				// compare info
				if ((nFileSize != wfdStructDest.nFileSizeLow) ||
					(wfdStruct.nFileSizeLow != wfdStructDest.nFileSizeLow) ||
					(wfdStruct.ftLastWriteTime.dwHighDateTime > wfdStructDest.ftLastWriteTime.dwHighDateTime) || // big time?
					// big times are equal, check little times
					((wfdStruct.ftLastWriteTime.dwLowDateTime == wfdStructDest.ftLastWriteTime.dwLowDateTime) &&
					(wfdStruct.ftLastWriteTime.dwLowDateTime > wfdStructDest.ftLastWriteTime.dwLowDateTime))
					)
				{
					SYSTEMTIME sysSrcTime;
					SYSTEMTIME sysDestTime;

					// there is a difference, copy data
					ODS(L"Difference detected: [%s%s]\n", pszCurSource, wfdStruct.cFileName);
					ODS(L"src sizes - wfd: %d nFileSize: %d dst size - wfd: %d\n",
						wfdStruct.nFileSizeLow, nFileSize, wfdStructDest.nFileSizeLow);

					FileTimeToSystemTime(&wfdStruct.ftLastWriteTime, &sysSrcTime);
					FileTimeToSystemTime(&wfdStructDest.ftLastWriteTime, &sysDestTime);
					ODS(L"times: - src time: %2.2d-%2.2d-%4.4d %2.2d:%2.2d:%2.2d.%3.3d - vs - dst time: %2.2d-%2.2d-%2.2d %2.2d:%2.2d:%2.2d.%3.3d\n",
						sysSrcTime.wMonth, sysSrcTime.wDay, sysSrcTime.wYear,
						sysSrcTime.wHour, sysSrcTime.wMinute, sysSrcTime.wSecond, sysSrcTime.wMilliseconds,
						sysDestTime.wMonth, sysDestTime.wDay, sysDestTime.wYear,
						sysDestTime.wHour, sysDestTime.wMinute, sysDestTime.wSecond, sysDestTime.wMilliseconds);

					// just overwrite it
					bPerformCopyPrune = 1;
				}

			}
			if (fDstFile) { fclose(fDstFile); fDstFile = 0; }
			if (hDstFindHandle) { FindClose(hDstFindHandle); hDstFindHandle = 0; }
		}
		else
		{
			bPerformCopyPrune = 1;
		}

		if (bPerformCopyPrune)
		{
			unsigned int nLength;

			// copy the file (+2 for \)
			pszNextSrc = (wchar_t*)malloc(sizeof(wchar_t) * (lstrlenW(pszCurSource) + lstrlenW(wfdStruct.cFileName) + 2));
			pszNextDst = (wchar_t*)malloc(sizeof(wchar_t) * (lstrlenW(pszCurDest) + lstrlenW(wfdStruct.cFileName) + 2));
			if (!pszNextSrc || !pszNextDst)
			{
				ReportSystemError((LPTSTR)"SourceDestScan", GetLastError());
				FATALODS(L"SourceScan: malloc failure");
			}
			memset(pszNextSrc, '\0', lstrlenW(pszCurSource) + lstrlenW(wfdStruct.cFileName) + 2);
			memset(pszNextDst, '\0', lstrlenW(pszCurDest) + lstrlenW(wfdStruct.cFileName) + 2);

			wsprintf(pszNextSrc, L"%s%s", pszCurSource, wfdStruct.cFileName);
			wsprintf(pszNextDst, L"%s%s", pszCurDest, wfdStruct.cFileName);

			if (gbPrune)
			{
				if (fSrcFile) { fclose(fSrcFile); fSrcFile = 0; }

				if (szDestRoot[0] != pszNextSrc[0])
				{
					FATALODS(L"ERROR, dir name mismatch! [%s vs %s]\n", szDestRoot, pszNextSrc);
				}

				ODS(L"Pruning %s\n", pszNextSrc);
				if (wfdStruct.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
				{
					wfdStruct.dwFileAttributes &= ~FILE_ATTRIBUTE_READONLY;     // turn this shit off
					SetFileAttributes(pszNextSrc, wfdStruct.dwFileAttributes);
				}

				bRetVal = DeleteFile(pszNextSrc);
			}
			else
			{
				if (szDestRoot[0] != pszNextDst[0])
				{
					FATALODS(L"ERROR, dir name mismatch! [%s vs %s]\n", szDestRoot, pszNextSrc);
				}

				ODS(L"Backing up %s to %s\n", pszNextSrc, pszNextDst);
				if (wfdStructDest.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
				{
					wfdStructDest.dwFileAttributes &= ~FILE_ATTRIBUTE_READONLY;     // turn this shit off
					SetFileAttributes(pszNextDst, wfdStructDest.dwFileAttributes);
				}

				bRetVal = CopyFile(pszNextSrc, pszNextDst, FALSE);
			}

			if (!bRetVal)
			{
				nGLE = GetLastError();
#if 0 //doesn't work
				if ((nGLE == ERROR_INVALID_NAME) && !gbPrune)
				{
					// try to manually open the file name

					// allocate space for the file contents
					pCopyBuffer = (PUCHAR)malloc(nFileSize);
					rewind(fSrcFile);
					fread(pCopyBuffer, 1, nFileSize, fSrcFile);

					// open it
					fDstFile = fopen(wfdStruct.cFileName, "wb");
					if (!fDstFile)
					{
						nGLE = GetLastError();
						ODS("SKIPPING! - File Update Failed: unable to open dest [%s%s] [GLE: %d]\n",
							pszCurDest, wfdStruct.cFileName, nGLE);
						free(pCopyBuffer);
						goto getnextfile;
					}
					fwrite(pCopyBuffer, 1, nFileSize, fDstFile);
					free(pCopyBuffer);
					goto getnextfile;
				}
#endif


				nLength = lstrlenW(pszNextSrc);
				if (nGLE == ERROR_PATH_NOT_FOUND)
				{
					// testing showed that the filename is usually something crazy (a cookie file)
					ODS(L"SKIPPING! - File Copy Failed: bad path [%s%s] [GLE: %d]\n",
						pszCurDest, wfdStruct.cFileName, nGLE);
				}
				else if (nGLE == ERROR_ACCESS_DENIED)
				{
					ODS(L"\nFile Copy Failed: access denied:\n[%s to\n%s]\n[path+filename len: %d]\n",
						pszNextSrc, pszNextDst, nLength);
				}
				else if (nGLE == ERROR_NO_SYSTEM_RESOURCES)
				{
					// we can get here if the file size is really big.  we need to use fopen/fread-fwrite instead
					ODS(L"\nFile Copy Failed: insufficent resouces: \n[%s to\n%s]\n[path+filename len: %d]\n",
						pszNextSrc, pszNextDst, nLength);
				}
				else
				{
					// 123 == ERROR_INVALID_NAME The filename, directory name, or volume label syntax is incorrect.
					ReportSystemError(L"Copy-Delete", nGLE);
					ODS(L"ERROR, Copy-Delete Failed [%s to %s] [path+filename len: %d] [GLE: %d]\n",
						pszNextSrc, pszNextDst, nLength, nGLE);
				}
			}

			bPerformCopyPrune = 0;
		}

	getnextfile:
		bPerformCopyPrune = 0;                   // reset copy prune
		SetCurrentDirectory(pszCurSource);      // set the dir back

		if (fSrcFile) { fclose(fSrcFile); fSrcFile = 0; }
		if (pszNextDst) { free(pszNextDst); pszNextDst = NULL; }
		if (pszNextSrc) { free(pszNextSrc); pszNextSrc = NULL; }

		// look for another file or dir
		bFindRetval = FindNextFile(hFindHandle, &wfdStruct);
	}

	if (hDstFindHandle) { FindClose(hDstFindHandle); hDstFindHandle = 0; }
	if (hFindHandle) { FindClose(hFindHandle); hFindHandle = 0; }
}


//char *gpSourceDir;
//char *gpDestDir;

#if 0
int GetSourceDir()
{
	OPENFILENAME ofn;
	int nRet;

	// prompt user for file name
	wsprintf(szCHPfile, "");
	ZeroMemory(&ofn, sizeof(OPENFILENAME));
	ofn.lStructSize = sizeof(OPENFILENAME);
	ofn.hwndOwner = hMainDlg;

	ofn.lpstrFilter = "CHP\0*.chp\0\0";
	ofn.nFilterIndex = 0;
	ofn.lpstrFile = szCHPfile;
	ofn.nMaxFile = sizeof(szCHPfile);
	ofn.lpstrFileTitle = NULL;
	ofn.lpstrTitle = "Open CHP File";
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST;

	nRet = GetOpenFileName(&ofn);
	if (nRet == 0)
	{
		MessageBox(NULL,
			"GetLoadFileName failed!",
			"PolyFScap",
			MB_ICONERROR);
		return -1;
	}

	return 0;
}
#endif

#if 0
void ParseCommandLine()
{
	for (int i = 1; i < __argc; i++)
	{
		LPCTSTR pszParam = __targv[i];
		BOOL bFlag = FALSE;
		BOOL bLast = ((i + 1) == __argc);
		if (pszParam[0] == '-' || pszParam[0] == '/')
		{
			// remove flag specifier
			bFlag = TRUE;
			++pszParam;
		}
		rCmdInfo.ParseParam(pszParam, bFlag, bLast);
	}
}
#endif

int main(int argc, char *argv[])
{
	wchar_t szLogfile[128];
	SYSTEMTIME sysTime;

	pOutputBuffer = (wchar_t*)malloc(sizeof(wchar_t) * MAXDIRFILENAME);
	memset(pOutputBuffer, '\0', MAXDIRFILENAME);

	GetCurrentDirectory(8192, szSourceRoot);
	ODS(L"Backup: current dir is %s\n", szSourceRoot);

	GetLocalTime(&sysTime);
	wsprintf(szLogfile, L"Backup-%2.2d-%2.2d-%4.4d_%2.2d.%2.2d.%2.2d.log",
		sysTime.wMonth, sysTime.wDay, sysTime.wYear, sysTime.wHour, sysTime.wMinute, sysTime.wSecond);
	fLogFile = _wfopen(szLogfile, L"wb");

#if 1
	//    nArgs = sscanf(lpCmdLine, "%4096s %4096s", szSourceRoot, szDestRoot);
	if (argc == 3)
	{
		// just source and dir
		wchar_t srcRoot[8192] = { 0 };
		MultiByteToWideChar(0, 0, argv[1], strlen(argv[1]), srcRoot, strlen(argv[1]));
		lstrcpynW(szSourceRoot, srcRoot, 8192);

		wchar_t destRoot[8192] = { 0 };
		MultiByteToWideChar(0, 0, argv[2], strlen(argv[2]), destRoot, strlen(argv[2]));
		lstrcpynW(szDestRoot, destRoot, 8192);
	}
	else if ((argc >= 5) && (argv[1][0] == '-') && (argv[1][1] == 'e'))
	{
		int i;

		if (argc > 19)
		{
			ODS(L"Backup only allows up to 16 excluded path names\n");
		}

		for (i = 2; i < argc - 2; i++)
		{
			int nLen;

			// 8191 just in case we need an extra char for the '\'
			wchar_t wcExcludeDirs[8192] = { 0 };
			MultiByteToWideChar(0, 0, argv[i], strlen(argv[i]), wcExcludeDirs, strlen(argv[i]));

			lstrcpynW(szExcludeDirs[i - 2], wcExcludeDirs, 8191);

			nLen = lstrlenW(szExcludeDirs[i - 2]);
			if (szExcludeDirs[i - 2][nLen - 1] != '\\')
			{
				szExcludeDirs[i - 2][nLen] = '\\';
				szExcludeDirs[i - 2][nLen + 1] = 0;
			}
			ODS(L"Excluding: %s\n", szExcludeDirs[i - 2]);
		}

		// just source and dir
		wchar_t szSourceRoot[8192];
		wchar_t szDestRoot[8192];

		wchar_t srcRoot[4096] = { 0 };
		MultiByteToWideChar(0, 0, argv[argc - 2], strlen(argv[argc - 2]), srcRoot, strlen(argv[argc - 2]));
		lstrcpynW(szSourceRoot, srcRoot, 4096);

		wchar_t destRoot[4096] = { 0 };
		MultiByteToWideChar(0, 0, argv[argc - 1], strlen(argv[argc - 1]), destRoot, strlen(argv[argc - 1]));
		lstrcpynW(szDestRoot, destRoot, 8192);

	}
	else
	{
		ODS(L"Usage: backup [-e <exclude dirs>] <src dir> <dest dir>\n");
	}

#else
	// check source
	strcpy(szSourceRoot, "C:\appz\Drivers\ATIdriver");
	strcpy(szDestRoot, "M:\\test");
	//  strcpy(szSourceRoot, "C:\\");
	//  strcpy(szDestRoot, "M:\\C_\\");
#endif
	ODS(L"WinMain: source: %s dest: %s\n", szSourceRoot, szDestRoot);

	SetAndCheckDir(szSourceRoot, TRUE);

	// check dest second so that SourceDestScan will scan dest first
	SetAndCheckDir(szDestRoot, TRUE);

	// scan the dest.  Check to see if files exist there that don't exist on the source
	// if they do then delete them
	gbPrune = 1;
	ODS(L"WinMain: pruning %s\n", szDestRoot);
	SourceDestScan(szDestRoot, szSourceRoot);

	// scan the source for files not yet copied to dest
	// perform copies as needed
	gbPrune = 0;
	SetAndCheckDir(szSourceRoot, TRUE);
	ODS(L"WinMain: backing up %s\n", szSourceRoot);
	SourceDestScan(szSourceRoot, szDestRoot);

	ODS(L"WinMain: Scan complete [files: %d] [dirs: %d]\n", nFileCount, nDirCount);

	fclose(fLogFile);
	free(pOutputBuffer);

	return 0;
}
