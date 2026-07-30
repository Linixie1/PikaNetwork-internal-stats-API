#pragma once

#include <string>
#include <Windows.h>
// backup attempt if LoadLibraryA fails, won't save the day either
namespace ProcessManager
{
    bool ManualMapDLL(DWORD processId, const char* dllPath, std::string& outError);
}