#include "folder.h"

#include <windows.h>
#include <filesystem>
#include <shlobj.h>

bool FolderManager::EnsureDirectoryExists(const std::string& path)
{
    if (!std::filesystem::exists(path)) {
        if (!CreateDirectoryA(path.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false; // if the .cutie dir already exsists in %userprofile% it skips creation
        }
    }
    return true;
}

std::string FolderManager::GetDocumentsPath(const std::string& subFolder)
{
    char path[MAX_PATH];

    // unused logic for documents folder instead of %userprofile%
    if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, path) == S_OK) {
        std::string fullPath = std::string(path) + "\\" + subFolder;

        if (!subFolder.empty() && !FolderManager::EnsureDirectoryExists(fullPath)) {
            return ""; // !dir creation
        }

        return fullPath;
    }
    else {
        return "";
    }
}

std::string FolderManager::GetDllPath()
{
	// checks if the file already exsists on the disk
	std::string exeDir = GetCurrentDir();
	std::string dllPath = exeDir + "\\cutie.dll";
	if (std::filesystem::exists(dllPath))
		return dllPath;
	return "";
}

std::string FolderManager::GetVersionStringDll()
{
	// finds the dll in the directory
	std::string exeDir = GetCurrentDir();
	std::string dllPath = exeDir + "\\cutie.dll";
	if (std::filesystem::exists(dllPath))
	{
		// returns hard-coded version tag
		return "127.0.0";
	}
	return "";
}

std::string FolderManager::GetCutieFolder()
{
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path) == S_OK) {
        std::string fullPath = std::string(path) + "\\.cutie";
        FolderManager::EnsureDirectoryExists(fullPath);
        return fullPath;
    }
    return "C:\\.cutie";
}

std::string FolderManager::GetCurrentDir() {
    char buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, buffer);
    return std::string(buffer);
}