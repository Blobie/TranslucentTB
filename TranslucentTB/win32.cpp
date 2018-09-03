#include "win32.hpp"
#include "arch.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <PathCch.h>
#include <processthreadsapi.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <sstream>
#include <synchapi.h>
#include <utility>
#include <WinBase.h>
#include <winerror.h>
#include <winnt.h>
#include <winrt/base.h>

#include "autofree.hpp"
#include "../CPicker/ccolourpicker.hpp"
#include "clipboardcontext.hpp"
#include "common.hpp"
#include "ttberror.hpp"
#include "ttblog.hpp"
#include "window.hpp"

const user32::pSetWindowCompositionAttribute user32::SetWindowCompositionAttribute =
	reinterpret_cast<pSetWindowCompositionAttribute>(
		GetProcAddress(GetModuleHandle(L"user32.dll"), "SetWindowCompositionAttribute")
	);

std::mutex win32::m_LocationLock;
std::wstring win32::m_ExeLocation;
std::mutex win32::m_PickerThreadsLock;
std::vector<DWORD> win32::m_PickerThreads;

DWORD win32::PickerThreadProc(LPVOID data)
{
	const HRESULT hr = CColourPicker(*reinterpret_cast<uint32_t *>(data)).CreateColourPicker();
	const DWORD tid = GetCurrentThreadId();
	{
		std::lock_guard guard(m_PickerThreadsLock);
		for (DWORD &i_tid : m_PickerThreads)
		{
			if (i_tid == tid)
			{
				std::swap(i_tid, m_PickerThreads.back());
				m_PickerThreads.pop_back();
				break;
			}
		}
	}
	if (FAILED(hr))
	{
		ErrorHandle(hr, Error::Level::Error, L"An error occured in the color picker!");
	}

	return 0;
}

BOOL win32::EnumThreadWindowsProc(HWND hwnd, LPARAM lParam)
{
	Window wnd(hwnd);
	bool &needs_wait = *reinterpret_cast<bool *>(lParam);

	if (*wnd.title() == L"Color Picker")
	{
		// 1068 == IDB_CANCEL
		wnd.send_message(WM_COMMAND, MAKEWPARAM(1068, BN_CLICKED));

		needs_wait = true;
		return false;
	}

	return true;
}

const std::wstring &win32::GetExeLocation()
{
	std::lock_guard guard(m_LocationLock);
	if (m_ExeLocation.empty())
	{
		DWORD exeLocation_size = LONG_PATH;
		std::wstring exeLocation;
		exeLocation.resize(exeLocation_size);
		if (QueryFullProcessImageName(GetCurrentProcess(), 0, exeLocation.data(), &exeLocation_size))
		{
			exeLocation.resize(exeLocation_size);
			m_ExeLocation = std::move(exeLocation);
		}
		else
		{
			LastErrorHandle(Error::Level::Fatal, L"Failed to determine executable location!");
		}
	}

	return m_ExeLocation;
}

bool win32::IsAtLeastBuild(const uint32_t &buildNumber)
{
	OSVERSIONINFOEX versionInfo = { sizeof(versionInfo), 10, 0, buildNumber };

	DWORDLONG mask = 0;
	VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
	VER_SET_CONDITION(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
	VER_SET_CONDITION(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

	if (VerifyVersionInfo(&versionInfo, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, mask))
	{
		return true;
	}
	else
	{
		DWORD error = GetLastError();
		if (error != ERROR_OLD_WIN_VERSION)
		{
			ErrorHandle(HRESULT_FROM_WIN32(error), Error::Level::Log, L"Error obtaining version info.");
		}

		return false;
	}
}

bool win32::IsSingleInstance()
{
	static winrt::handle mutex;

	if (!mutex)
	{
		mutex = CreateMutex(NULL, FALSE, MUTEX_GUID);
		LRESULT error = GetLastError();
		switch (error)
		{
		case ERROR_ALREADY_EXISTS:
			return false;
			break;

		case ERROR_SUCCESS:
			return true;
			break;

		default:
			ErrorHandle(HRESULT_FROM_WIN32(error), Error::Level::Error, L"Failed to open app mutex!");
			return true;
		}
	}
	else
	{
		return true;
	}
}

bool win32::IsDirectory(const std::wstring &directory)
{
	DWORD attributes = GetFileAttributes(directory.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		DWORD error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
		{
			// This function gets called during log initialization, so avoid potential recursivity
			ErrorHandle(HRESULT_FROM_WIN32(error), Error::Level::Debug, L"Failed to check if directory exists.");
		}
		return false;
	}
	else
	{
		return attributes & FILE_ATTRIBUTE_DIRECTORY;
	}
}

bool win32::FileExists(const std::wstring &file)
{
	DWORD attributes = GetFileAttributes(file.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		DWORD error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
		{
			ErrorHandle(HRESULT_FROM_WIN32(error), Error::Level::Log, L"Failed to check if file exists.");
		}
		return false;
	}
	else
	{
		return !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}
}

bool win32::CopyToClipboard(std::wstring_view text)
{
	ClipboardContext context;
	if (!context)
	{
		LastErrorHandle(Error::Level::Error, L"Failed to open clipboard.");
		return false;
	}

	if (!EmptyClipboard())
	{
		LastErrorHandle(Error::Level::Error, L"Failed to empty clipboard.");
		return false;
	}

	auto data = AutoFree::GlobalHandle<wchar_t[]>::Alloc(text.length() + 1);
	if (!data)
	{
		LastErrorHandle(Error::Level::Error, L"Failed to allocate memory for the clipboard.");
		return false;
	}

	{
		AutoFree::GlobalLock lock(data);
		if (!lock)
		{
			LastErrorHandle(Error::Level::Error, L"Failed to lock memory for the clipboard.");
			return false;
		}
		text.copy(lock.get(), text.length());
	}

	if (!SetClipboardData(CF_UNICODETEXT, data.detach()))
	{
		LastErrorHandle(Error::Level::Error, L"Failed to copy data to clipboard.");
		return false;
	}

	return true;
}

void win32::EditFile(const std::wstring &file)
{
	SHELLEXECUTEINFO info = {
		sizeof(info),		// cbSize
		SEE_MASK_CLASSNAME,	// fMask
		NULL,				// hwnd
		L"open",			// lpVerb
		file.c_str(),		// lpFile
		NULL,				// lpParameters
		NULL,				// lpDirectory
		SW_SHOW,			// nShow
		nullptr,			// hInstApp
		nullptr,			// lpIDList
		L"txtfile"			// lpClass
	};

	if (!ShellExecuteEx(&info))
	{
		std::thread([file]
		{
			std::wstring boxbuffer =
				L"Failed to open file \"" + file + L"\"." +
				L"\n\n" + Error::ExceptionFromHRESULT(HRESULT_FROM_WIN32(GetLastError())) +
				L"\n\nCopy the file location to the clipboard?";

			if (MessageBox(Window::NullWindow, boxbuffer.c_str(), NAME L" - Error", MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND) == IDYES)
			{
				CopyToClipboard(file);
			}
		}).detach();
	}
}

void win32::OpenLink(const std::wstring &link)
{
	SHELLEXECUTEINFO info = {
		sizeof(info),							// cbSize
		SEE_MASK_CLASSNAME,						// fMask
		NULL,									// hwnd
		L"open",								// lpVerb
		link.c_str(),							// lpFile
		NULL,									// lpParameters
		NULL,									// lpDirectory
		SW_SHOW,								// nShow
		nullptr,								// hInstApp
		nullptr,								// lpIDList
		link[4] == L's' ? L"https" : L"http"	// lpClass
	};

	if (!ShellExecuteEx(&info))
	{
		std::thread([link]
		{
			std::wstring boxbuffer =
				L"Failed to open URL \"" + link + L"\"." +
				L"\n\n" + Error::ExceptionFromHRESULT(HRESULT_FROM_WIN32(GetLastError())) +
				L"\n\nCopy the URL to the clipboard?";

			if (MessageBox(Window::NullWindow, boxbuffer.c_str(), NAME L" - Error", MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND) == IDYES)
			{
				CopyToClipboard(link);
			}
		}).detach();
	}
}

DWORD win32::PickColor(uint32_t &color)
{
	DWORD threadId;
	const winrt::handle hThread = CreateThread(nullptr, 0, PickerThreadProc, &color, CREATE_SUSPENDED, &threadId);

	if (hThread)
	{
		{
			std::lock_guard guard(m_PickerThreadsLock);
			m_PickerThreads.emplace_back(threadId);
		}

		ResumeThread(hThread.get());
		return threadId;
	}
	else
	{
		LastErrorHandle(Error::Level::Error, L"Failed to spawn color picker thread!");
		return 0;
	}
}

void win32::ClosePickers()
{
	std::unique_lock guard(m_PickerThreadsLock);
	while (m_PickerThreads.size() != 0)
	{
		const DWORD tid = *m_PickerThreads.begin();
		bool needs_wait = false;
		guard.unlock();
		EnumThreadWindows(tid, EnumThreadWindowsProc, reinterpret_cast<LPARAM>(&needs_wait));

		if (needs_wait)
		{
			const winrt::handle thread = OpenThread(SYNCHRONIZE, FALSE, tid);
			if (thread)
			{
				WaitForSingleObject(thread.get(), INFINITE);
			}
		}

		guard.lock();
	}
}

void win32::HardenProcess()
{
	PROCESS_MITIGATION_ASLR_POLICY aslr_policy;
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr_policy, sizeof(aslr_policy)))
	{
		aslr_policy.EnableForceRelocateImages = true;
		aslr_policy.DisallowStrippedImages = true;
		if (!SetProcessMitigationPolicy(ProcessASLRPolicy, &aslr_policy, sizeof(aslr_policy)))
		{
			LastErrorHandle(Error::Level::Log, L"Couldn't disallow stripped images.");
		}
	}
	else
	{
		LastErrorHandle(Error::Level::Log, L"Couldn't get current ASLR policy.");
	}

	PROCESS_MITIGATION_DYNAMIC_CODE_POLICY code_policy {};
	code_policy.ProhibitDynamicCode = true;
	code_policy.AllowThreadOptOut = false;
	code_policy.AllowRemoteDowngrade = false;
	if (!SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &code_policy, sizeof(code_policy)))
	{
		LastErrorHandle(Error::Level::Log, L"Couldn't disable dynamic code generation.");
	}

	PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handle_policy {};
	handle_policy.RaiseExceptionOnInvalidHandleReference = true;
	handle_policy.HandleExceptionsPermanentlyEnabled = true;
	if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &handle_policy, sizeof(handle_policy)))
	{
		LastErrorHandle(Error::Level::Log, L"Couldn't enable strict handle checks.");
	}

	PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extension_policy {};
	extension_policy.DisableExtensionPoints = true;
	if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extension_policy, sizeof(extension_policy)))
	{
		LastErrorHandle(Error::Level::Log, L"Couldn't disable extension point DLLs.");
	}

	PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature_policy {};
	signature_policy.MitigationOptIn = true;
	if (!SetProcessMitigationPolicy(ProcessSignaturePolicy, &signature_policy, sizeof(signature_policy)))
	{
		LastErrorHandle(Error::Level::Log, L"Couldn't enable image signature enforcement.");
	}


	PROCESS_MITIGATION_IMAGE_LOAD_POLICY load_policy {};
	load_policy.NoLowMandatoryLabelImages = true;
	load_policy.PreferSystem32Images = true;

	// https://blogs.msdn.microsoft.com/oldnewthing/20160602-00/?p=93556
	std::vector<wchar_t> volumePath(LONG_PATH);
	if (GetVolumePathName(GetExeLocation().c_str(), volumePath.data(), LONG_PATH))
	{
		load_policy.NoRemoteImages = GetDriveType(volumePath.data()) != DRIVE_REMOTE;
	}
	else
	{
		LastErrorHandle(Error::Level::Log, L"Unable to get volume path name.");
	}

	if (!SetProcessMitigationPolicy(ProcessImageLoadPolicy, &load_policy, sizeof(load_policy)))
	{
		LastErrorHandle(Error::Level::Log, L"Couldn't set image load policy.");
	}
}

std::wstring win32::CharToWchar(const char *const str)
{
	const size_t strLength = std::char_traits<char>::length(str);
	std::wstring strW;
	strW.resize(strLength);
	int count = MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, str, strLength, strW.data(), strLength);
	if (count)
	{
		strW.resize(count);
	}
	else
	{
		strW.erase();
	}

	return strW;
}

std::pair<std::wstring, HRESULT> win32::GetWindowsBuild()
{
	// Microsoft recommends this themselves
	// https://docs.microsoft.com/en-us/windows/desktop/SysInfo/getting-the-system-version
	AutoFree::CoTaskMem<wchar_t[]> system32;
	HRESULT hr = SHGetKnownFolderPath(FOLDERID_System, KF_FLAG_DEFAULT, NULL, system32.put());
	if (FAILED(hr))
	{
		return { L"", hr };
	}

	AutoFree::Local<wchar_t[]> kernel32;
	hr = PathAllocCombine(system32.get(), L"kernel32.dll", PATHCCH_ALLOW_LONG_PATHS, kernel32.put());
	if (FAILED(hr))
	{
		return { L"", hr };
	}

	return GetFileVersion(kernel32.get());
}

std::pair<std::wstring, HRESULT> win32::GetFileVersion(const std::wstring &file)
{
	DWORD thisisuseless;
	DWORD size = GetFileVersionInfoSize(file.c_str(), &thisisuseless);
	if (!size)
	{
		return { L"", HRESULT_FROM_WIN32(GetLastError()) };
	}

	auto data = std::make_unique<std::byte[]>(size);
	if (!GetFileVersionInfo(file.c_str(), thisisuseless, size, data.get()))
	{
		return { L"", HRESULT_FROM_WIN32(GetLastError()) };
	}

	wchar_t *fileVersion;
	unsigned int length;
	if (!VerQueryValue(data.get(), LR"(\StringFileInfo\040904b0\FileVersion)", reinterpret_cast<void **>(&fileVersion), &length))
	{
		return { L"", HRESULT_FROM_WIN32(GetLastError()) };
	}

	return { fileVersion, S_OK };
}

unsigned long long win32::FiletimeToUnixEpoch(const FILETIME &time)
{
	unsigned long long timeStamp = ULARGE_INTEGER{{ time.dwLowDateTime, time.dwHighDateTime }}.QuadPart;

	// FILETIME is in hundreds of nanoseconds, but Unix timestamps are in seconds.
	timeStamp /= 10000000;

	// Unix timestamps are since 1970, but FILETIME is since 1601.
	// Black magic told me there are 11644473600 seconds between the two years.
	timeStamp -= 11644473600;

	return timeStamp;
}