#pragma once

#include <vector>
#include <Windows.h>
#include <string>

namespace ProcessManager
{
	struct WindowInfo
	{
		WindowInfo(DWORD processId, const std::string& processName, HWND hwnd)
			: processId(processId), processName(processName), hwnd(hwnd) {}
		DWORD processId;
		std::string processName;
		HWND hwnd;
	};

	bool GetMinecraftProcesses(std::vector<WindowInfo>& processes);
	bool InjectDLL(DWORD processId, const char* dllPath, std::string& outError);
	bool EjectStaleDLL(DWORD processId, std::string& outError);
	bool ExtractEmbeddedPayloads();
	std::string GetDllPath();
	std::string GetProcessName(DWORD processId);

	// gets creation time
	FILETIME GetProcessCreationTime(DWORD processId);

	// gets uptime
	ULONGLONG GetProcessUptimeMs(DWORD processId);

	inline static std::vector<WindowInfo> _processes;
	inline static std::string s_embeddedDllPath;
}
