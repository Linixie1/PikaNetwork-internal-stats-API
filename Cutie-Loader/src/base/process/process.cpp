#include "process.h"

#include <Psapi.h>
#include <string>
#include <TlHelp32.h>
#include <algorithm>

#include <iostream>
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <winternl.h>
#include <chrono>

#include "../util/log.h"
#include "../../../resource.h"

static bool IsMinecraftWindow(HWND hwnd, DWORD pid) {
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    std::wstring titleStr(title);

    // launchers to exclude from injecting as they have jvm.dll and opengl32.dll, while not being the target
    if (titleStr.find(L"TLauncher") != std::wstring::npos) return false;
    if (titleStr.find(L"tlauncher") != std::wstring::npos) return false;
    if (titleStr.find(L"Electric") != std::wstring::npos) return false;
    if (titleStr.find(L"Launcher") != std::wstring::npos) return false;

    auto hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me = { sizeof(me) };
    bool hasJVM = false, hasLWJGL = false;
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"jvm.dll") == 0) hasJVM = true;
            if (_wcsicmp(me.szModule, L"lwjgl.dll") == 0 || _wcsicmp(me.szModule, L"lwjgl64.dll") == 0) hasLWJGL = true;
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    return hasJVM && hasLWJGL;
}

static std::string GetCutieFolder() {
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path) == S_OK) {
        std::string fullPath = std::string(path) + "\\.cutie\\";
        if (!std::filesystem::exists(fullPath))
            std::filesystem::create_directories(fullPath);
        return fullPath;
    }
    return "C:\\.cutie\\";
}

// gets the directory of the loader
static std::string GetExeDir() {
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (len == 0) return "";
    std::string path(buffer, len);
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        path = path.substr(0, pos);
    return path;
}

static bool ExtractResourceToFile(int resId, LPCWSTR resType, const char* outPath) {
    HMODULE hMod = GetModuleHandle(NULL);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resId), resType);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hMod, hRes);
    if (!hData) return false;
    DWORD size = SizeofResource(hMod, hRes);
    LPVOID pData = LockResource(hData);
    if (!pData || size == 0) return false;

    // check if the file is already extracted, if so, skip extraction, otherwise extract it
    if (std::filesystem::exists(outPath)) {
        if (std::filesystem::file_size(outPath) == size) {
            std::ifstream in(outPath, std::ios::binary);
            std::vector<char> buffer(size);
            if (in.read(buffer.data(), size)) {
                if (memcmp(buffer.data(), pData, size) == 0) {
                    return true;
                }
            }
        }
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write((const char*)pData, size);
    out.close();
    return true;
}

bool ProcessManager::ExtractEmbeddedPayloads() {
    Logger& log = Logger::GetInstance();

    // extracts the resource in the loaders directory (from where LoadLibraryA is called, or manual mapping if stealthier in the enviornment)
    std::string exeDir = GetExeDir();
    if (exeDir.empty()) {
        log.Write("[extract] ERROR: failed to get exe directory, skipping extraction 1");
        return false;
    }

    s_embeddedDllPath = exeDir + "\\cutie.dll";

    // overwrites the shellcode reservoir, to make sure it's the latest version
    if (!ExtractResourceToFile(IDR_DLL1, RT_RCDATA, s_embeddedDllPath.c_str())) {
        log.Write("[extract] ERROR: extraction of the shellcode reservoir failed to " + s_embeddedDllPath);
        return false;
    }
    log.Write("[extract] extracted shellcode reservoir to " + s_embeddedDllPath);

    // create configs folder in the .cutie folder to save configs and load the deafult config
    std::string cutieFolder = GetCutieFolder();
    std::string configsDir = cutieFolder + "configs\\";
    if (!std::filesystem::exists(configsDir))
        std::filesystem::create_directories(configsDir);

    log.Write("[extract] configs directory: " + configsDir);

    std::string configPath = configsDir + "default.cutie";
    if (!std::filesystem::exists(configPath))
        ExtractResourceToFile(IDR_CONFIG1, RT_RCDATA, configPath.c_str());

    std::string statsCfgPath = configsDir + "cutie-stats.cfg";
    if (!std::filesystem::exists(statsCfgPath))
        ExtractResourceToFile(IDR_STATS_CFG, RT_RCDATA, statsCfgPath.c_str());

    return std::filesystem::exists(s_embeddedDllPath);
}

std::string ProcessManager::GetDllPath() {
    if (!s_embeddedDllPath.empty() && std::filesystem::exists(s_embeddedDllPath))
        return s_embeddedDllPath;

    // basic fallback
    std::string exeDir = GetExeDir();
    if (!exeDir.empty()) {
        std::string fallbackPath = exeDir + "\\cutie.dll";
        if (std::filesystem::exists(fallbackPath))
            return fallbackPath;
    }

    return "";
}

bool ProcessManager::GetMinecraftProcesses(std::vector<WindowInfo>& processes)
{
	ProcessManager::_processes.clear();
	processes.clear();

	EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
		DWORD PID = 0;
		GetWindowThreadProcessId(hwnd, &PID);

		const static DWORD TITLE_SIZE = 1024;
		CHAR windowTitle[TITLE_SIZE];
		GetWindowTextA(hwnd, windowTitle, TITLE_SIZE);
		const int win_name_length = GetWindowTextLength(hwnd);

		if (IsWindowVisible(hwnd) && win_name_length != 0) {
			auto title = std::string(windowTitle);

			if (IsMinecraftWindow(hwnd, PID))
			{
				ProcessManager::_processes.emplace_back(PID, title, hwnd);
			}

			return TRUE;
		}

		return TRUE;
		}, NULL);

	std::sort(ProcessManager::_processes.begin(), ProcessManager::_processes.end(), [](const WindowInfo& a, const WindowInfo& b) {
		return a.processName < b.processName;
		});

	processes = ProcessManager::_processes;
    return true;
}

bool ProcessManager::EjectStaleDLL(DWORD processId, std::string& outError)
{
	// forcefully unloads any leftovers of the PE image before allocating again (if FreeLibraryAndExitThread fails duo to hooks)

	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
	if (hSnap == INVALID_HANDLE_VALUE) {
		outError = "EjectStaleDLL: Failed to snapshot target process. Error: " + std::to_string(GetLastError());
		return false;
	}

	struct FoundDLL {
		HMODULE hModule = NULL;
		std::wstring name;
	};
	std::vector<FoundDLL> staleModules;

	MODULEENTRY32W me = { sizeof(me) };
	if (Module32FirstW(hSnap, &me)) {
		do {
			std::wstring moduleName(me.szModule);
			std::wstring exePath(me.szExePath);

			// looks for any cutie or DLL PE image
			if (moduleName.find(L"cutie") != std::wstring::npos &&
				moduleName.find(L".dll") != std::wstring::npos) {
				FoundDLL found;
				found.hModule = me.hModule;
				found.name = moduleName;
				staleModules.push_back(found);
			}
		} while (Module32NextW(hSnap, &me));
	}
	CloseHandle(hSnap);

	if (staleModules.empty()) {
		// returns safe unloading 1
		return true;
	}

	// attempts to open the target process
	HANDLE hProcess = OpenProcess(
		PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
		PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
		FALSE, processId
	);
	if (!hProcess) {
		outError = "EjectStaleDLL: Failed to open target process. Error: " + std::to_string(GetLastError());
		return false;
	}

	struct ProcessGuard { HANDLE h; ~ProcessGuard() { if (h) CloseHandle(h); } } guard{hProcess};

	// Get FreeLibrary address from the target process's kernel32, 
    // kernel32 is mapped at the same base in all processes in a session

	HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
	LPTHREAD_START_ROUTINE freeLibraryAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hKernel32, "FreeLibrary"));
	if (!freeLibraryAddr) {
		outError = "EjectStaleDLL: Failed to find FreeLibrary address. 0";
		return false;
	}

    // attempts to forcefully unload any loaded DLL (bad practice)
	for (auto& stale : staleModules) {
		// call FreeLibrary up to 64 times to guarantee refcount hits 0.
		// sleeps 50ms between calls to give the process time to process
		for (int attempt = 0; attempt < 64; ++attempt) {
			HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, freeLibraryAddr, stale.hModule, 0, NULL);
			if (hThread) {
				WaitForSingleObject(hThread, 2000);
				DWORD exitCode = 0;
				GetExitCodeThread(hThread, &exitCode);
				CloseHandle(hThread);

				// If FreeLibrary returned 0, the module is fully unloaded or refcount hit 0 and we can stop trying to unload it earlier
				if (exitCode == 0) {
					break;
				}
			}
			Sleep(50);
		}
	}

	// verifies the ejection by re-snapshotting a few times
	bool stillPresent = false;
	for (int verifyAttempt = 0; verifyAttempt < 5; ++verifyAttempt) {
		Sleep(500); // gives the process time to unload without crashing it

		hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
		stillPresent = false;
		if (hSnap != INVALID_HANDLE_VALUE) {
			me = { sizeof(me) };
			if (Module32FirstW(hSnap, &me)) {
				do {
					std::wstring moduleName(me.szModule);
					if (moduleName.find(L"cutie") != std::wstring::npos &&
						moduleName.find(L".dll") != std::wstring::npos) {
						stillPresent = true;
						break;
					}
				} while (Module32NextW(hSnap, &me));
			}
			CloseHandle(hSnap);
		}

		if (!stillPresent) break;
	}

	if (stillPresent) {
		outError = "EjectStaleDLL: Cutie DLL is still mapped in Minecraft after 64 FreeLibrary calls.\n\n"
				   "This means the DLL has active MinHook trampolines or Java threads \n"
				   "that prevent Windows from unloading it. Restart Minecraft to clear it,\n"
				   "then try injecting again.";
		return false;
	}

	return true;
}


// NtCreateThreadEx prototype (*3x CFG on Windows 11)

typedef NTSTATUS(NTAPI* pNtCreateThreadEx)(
    OUT PHANDLE ThreadHandle,
    IN ACCESS_MASK DesiredAccess,
    IN PVOID ObjectAttributes,
    IN HANDLE ProcessHandle,
    IN PVOID StartAddress,
    IN PVOID Parameter,
    IN BOOL CreateSuspended,
    IN ULONG StackZeroBits,
    IN ULONG SizeOfStackCommit,
    IN ULONG SizeOfStackReserve,
    OUT PVOID BytesBuffer
    );

bool ProcessManager::InjectDLL(DWORD processId, const char* dllPath, std::string& outError)
{
    if (!dllPath || strlen(dllPath) == 0) {
        outError = "DLL path is empty.";
        return false;
    }

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
    if (hProcess == NULL) {
        outError = "Failed to open target process. Error: " + std::to_string(GetLastError());
        return false;
    }

    struct ProcessGuard { HANDLE h; ~ProcessGuard() { if (h) CloseHandle(h); } } guard{hProcess};

    SIZE_T pathLen = strlen(dllPath) + 1;
    LPVOID allocatedMemory = VirtualAllocEx(hProcess, NULL, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (allocatedMemory == NULL) {
        outError = "Failed to allocate memory in target process. Error: " + std::to_string(GetLastError());
        return false;
    }

    if (!WriteProcessMemory(hProcess, allocatedMemory, dllPath, pathLen, NULL)) {
        outError = "Failed to write memory in target process. Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
        return false;
    }

    LPVOID loadLibraryAddr = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (loadLibraryAddr == NULL) {
        outError = "Failed to find LoadLibraryA address. Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
        return false;
    }

    // Use NtCreateThreadEx instead of CreateRemoteThread, as it's more stable duo to CFG on Windows 11
    pNtCreateThreadEx NtCreateThreadEx = (pNtCreateThreadEx)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx");
    if (!NtCreateThreadEx) {
        outError = "Failed to find NtCreateThreadEx in ntdll.dll.";
        VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = NULL;
    NTSTATUS status = NtCreateThreadEx(&hThread, THREAD_ALL_ACCESS, NULL, hProcess,
                                       loadLibraryAddr, allocatedMemory,
                                       FALSE, 0, 0, 0, NULL);

    // fall back to CreateRemoteThread if NtCreateThreadEx fails (generally won't work either if NtCreateThreadEx fails)
    if (!NT_SUCCESS(status) || hThread == NULL) {
        hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryAddr, allocatedMemory, 0, NULL);
    }
    
    // if both fail, log the issue, it will not work.
    if (hThread == NULL) {
        outError = "Failed to create remote thread (both NtCreateThreadEx and CreateRemoteThread failed). Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
        return false;
    }

    DWORD waitResult = WaitForSingleObject(hThread, 10000);
    if (waitResult == WAIT_TIMEOUT) {
        outError = "Injection thread timed out.";
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
        return false;
    } else if (waitResult == WAIT_FAILED) {
        outError = "Failed waiting for injection thread. Error: " + std::to_string(GetLastError());
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
        return false;
    }

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, allocatedMemory, 0, MEM_RELEASE);
    // hProcess closed by ProcessGuard RAII

    if (exitCode == STILL_ACTIVE) {
        outError = "Injection thread is still active.";
        return false;
    } else if (exitCode == 0) {
        outError = "LoadLibraryA failed in the target process (returned NULL).\n\n"
                   "This may be caused by:\n"
                   " - Antivirus/Defender blocking the injection\n"
                   " - Windows Smart App Control preventing DLL loads\n"
                   " - The DLL is corrupted or has missing dependencies (unlikely)";
        return false;
    } else if (exitCode >= 0xC0000000) {
        char hexCode[32];
        sprintf_s(hexCode, "0x%08X", exitCode);
        outError = std::string("Injection blocked by Windows Security (CFG/Defender). Thread crashed: ") + hexCode;
        return false;
    }

    // LoadLibraryA returned a valid module handle, meaning the PE image is loaded correctly
    return true;
}

FILETIME ProcessManager::GetProcessCreationTime(DWORD processId)
{
    FILETIME zero = { 0, 0 };

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (!hProcess)
        return zero;

    FILETIME creationTime, exitTime, kernelTime, userTime;
    BOOL ok = GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime);
    CloseHandle(hProcess);

    if (!ok)
        return zero;

    return creationTime;
}

// returns the process uptime in milliseconds to avoid loading a module and crashing a fresh JVM
ULONGLONG ProcessManager::GetProcessUptimeMs(DWORD processId)
{
    FILETIME creationTime = GetProcessCreationTime(processId);
    if (creationTime.dwLowDateTime == 0 && creationTime.dwHighDateTime == 0)
        return 0;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);

    ULARGE_INTEGER liCreation, liNow;
    liCreation.LowPart = creationTime.dwLowDateTime;
    liCreation.HighPart = creationTime.dwHighDateTime;
    liNow.LowPart = now.dwLowDateTime;
    liNow.HighPart = now.dwHighDateTime;


    ULONGLONG diff = liNow.QuadPart - liCreation.QuadPart;


    return diff / 10000ULL;
}
