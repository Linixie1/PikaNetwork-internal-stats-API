#include <iostream>
#include <windows.h>
#include <shlobj.h>
#include <filesystem>

#include "base.h"

static bool MinecraftFolderExists() {
    char appData[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData) != S_OK)
        return false;
    std::string mcPath = std::string(appData) + "\\.minecraft";
    return std::filesystem::exists(mcPath);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Prevent multiple instances
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Global\\cutie-loader-mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex) CloseHandle(hMutex);
        MessageBoxA(NULL,
            "cutie loader is already running.\n\n"
            "Please close the existing loader before launching a new one.",
            "cutie",
            MB_OK | MB_ICONWARNING);
        return 0;
    }

    if (!MinecraftFolderExists()) {
        MessageBoxA(NULL, "Minecraft not found. Please install Minecraft first.", "Cutie Loader", MB_OK | MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    Base base;
    base.Run();
    CloseHandle(hMutex);
    return 0;
}
 