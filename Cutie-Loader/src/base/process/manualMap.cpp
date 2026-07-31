// unused code - LoadLibraryA is more reliable, not called in <ClCompile>, not referenced in Cutie-Loader.vcxproj.filters
#include "manualMap.h"

#include <windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>

#include "../util/log.h"

namespace {


// reads the entire PE image from disk into a byte vector

std::vector<uint8_t> ReadDllFile(const char* path, std::string& outError) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        outError = "ManualMap: Failed to open DLL file: " + std::string(path);
        return {};
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        outError = "ManualMap: Failed to read DLL file: " + std::string(path);
        return {};
    }

    return buffer;
}


// find a module in the target process and get its base address

uint64_t GetRemoteModuleBase(HANDLE hProcess, const wchar_t* moduleName, std::string& outError) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(hProcess));
    if (hSnap == INVALID_HANDLE_VALUE) {
        outError = "ManualMap: Failed to snapshot target process modules. Error: " + std::to_string(GetLastError());
        return 0;
    }

    uint64_t base = 0;
    MODULEENTRY32W me = { sizeof(me) };

    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) {
                base = reinterpret_cast<uint64_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }

    CloseHandle(hSnap);

    if (base == 0) {
        char nameBuf[256] = {};
        WideCharToMultiByte(CP_ACP, 0, moduleName, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
        outError = "ManualMap: Module not found in target: " + std::string(nameBuf);
    }
    return base;
}


// get the LoadLibraryA and GetProcAddress addresses from the target process
// these are at the same address in all processes for kernel32.dll

uint64_t GetRemoteLoadLibraryA(HANDLE hProcess, std::string& outError) {
    // kernel32.dll is mapped at the same base address across all processes
    // on the same session, so we can use our local GetProcAddress value
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        outError = "ManualMap: Failed to get kernel32.dll handle.";
        return 0;
    }

    FARPROC addr = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!addr) {
        outError = "ManualMap: Failed to find LoadLibraryA.";
        return 0;
    }

    return reinterpret_cast<uint64_t>(addr);
}

uint64_t GetRemoteGetProcAddress(HANDLE hProcess, std::string& outError) {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        outError = "ManualMap: Failed to get kernel32.dll handle.";
        return 0;
    }

    FARPROC addr = GetProcAddress(hKernel32, "GetProcAddress");
    if (!addr) {
        outError = "ManualMap: Failed to find GetProcAddress.";
        return 0;
    }

    return reinterpret_cast<uint64_t>(addr);
}

// x64 shellcode that calls DllMain(DLL_PROCESS_ATTACH, NULL)
// uses position-independent code with embedded addresses patched at runtime
// The shellcode:
//   1. Calls DllMain(hinstDLL, DLL_PROCESS_ATTACH, NULL)
//   2. Returns DllMain's return value for further usage
#pragma pack(push, 1)
struct Shellcode64 {
    // sub rsp, 0x28 (shadow space + alignment)
    uint8_t subRsp[4] = { 0x48, 0x83, 0xEC, 0x28 };

    // mov rcx, <ImageBase>
    uint8_t movRcx[2] = { 0x48, 0xB9 };
    uint64_t imageBase = 0;       // offset 6 

    // mov edx, 1 (DLL_PROCESS_ATTACH)
    uint8_t movEdx[5] = { 0xBA, 0x01, 0x00, 0x00, 0x00 };

    // xor r8, r8 (lpvReserved = NULL)
    uint8_t xorR8[3] = { 0x4D, 0x31, 0xC0 };

    // mov rax, <EntryPoint>
    uint8_t movRax[2] = { 0x48, 0xB8 };
    uint64_t entryPoint = 0;      // offset 24

    // call rax
    uint8_t callRax[2] = { 0xFF, 0xD0 };

    // add rsp, 0x28
    uint8_t addRsp[4] = { 0x48, 0x83, 0xC4, 0x28 };

    // ret
    uint8_t ret = 0xC3;
};
#pragma pack(pop)

} // anonymous namespace


bool ProcessManager::ManualMapDLL(DWORD processId, const char* dllPath, std::string& outError)
{
    Logger& log = Logger::GetInstance();

    // Step 1: Read DLL file into buffer 
    log.Write("[mapper] step 1: reading DLL from disk");
    auto dllBuffer = ReadDllFile(dllPath, outError);
    if (dllBuffer.empty()) return false;
    log.Write("[mapper] step 1 done: " + std::to_string(dllBuffer.size()) + " bytes");

    // Step 2: Parse PE headers
    log.Write("[mapper] step 2: parsing PE headers");
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(dllBuffer.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        outError = "ManualMap: Invalid PE file (bad DOS signature).";
        return false;
    }

    IMAGE_NT_HEADERS64* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(dllBuffer.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        outError = "ManualMap: Invalid PE file (bad NT signature).";
        return false;
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        outError = "ManualMap: Only x64 DLLs are supported (got Magic=0x" +
            []() { char buf[16]; sprintf_s(buf, "%04X", IMAGE_NT_OPTIONAL_HDR64_MAGIC); return std::string(buf); }() + ").";
        return false;
    }

    uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    uint32_t headersSize = nt->OptionalHeader.SizeOfHeaders;
    uint64_t preferredBase = nt->OptionalHeader.ImageBase;
    log.Write("[mapper] step 2 done: imageSize=" + std::to_string(imageSize) + " headersSize=" + std::to_string(headersSize));

    // Step 3: Open target process
    log.Write("[mapper] step 3: opening target process PID=" + std::to_string(processId));
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, processId
    );
    if (!hProcess) {
        outError = "ManualMap: Failed to open target process. Error: " + std::to_string(GetLastError());
        return false;
    }
    log.Write("[mapper] step 3 done");

    struct ProcessGuard { HANDLE h; ~ProcessGuard() { if (h) CloseHandle(h); } } guard{hProcess};

    // Step 4: Allocate memory in target process for the image
    log.Write("[mapper] step 4: allocating " + std::to_string(imageSize) + " bytes in target");
    LPVOID remoteBase = VirtualAllocEx(hProcess,
        reinterpret_cast<LPVOID>(preferredBase),
        imageSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);  // FIX: No RWX!!! RW is sufficient 

    if (!remoteBase) {
        remoteBase = VirtualAllocEx(hProcess, NULL, imageSize,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // same as above
    }

    if (!remoteBase) {
        outError = "ManualMap: Failed to allocate " + std::to_string(imageSize) +
                   " bytes in target process. Error: " + std::to_string(GetLastError());
        return false;
    }

    log.Write("[mapper] step 4 done: remoteBase=0x" + []() {
        char buf[32]; sprintf_s(buf, "%p", (void*)0); return std::string(buf);
    }() + std::to_string(reinterpret_cast<uintptr_t>(remoteBase)));

    uint64_t delta = reinterpret_cast<uint64_t>(remoteBase) - preferredBase;
    SIZE_T bytesWritten = 0;

    // Step 5: Copy PE headers
    log.Write("[mapper] step 5: writing headers");
    if (!WriteProcessMemory(hProcess, remoteBase, dllBuffer.data(), headersSize, &bytesWritten)) {
        outError = "ManualMap: Failed to write PE headers. Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }
    log.Write("[mapper] step 5 done");

    // Step 6: Copy sections
    log.Write("[mapper] step 6: copying sections");
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->SizeOfRawData == 0) continue;

        uint8_t* src = dllBuffer.data() + section->PointerToRawData;
        uint64_t dstAddr = reinterpret_cast<uint64_t>(remoteBase) + section->VirtualAddress;

        if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(dstAddr), src,
                                section->SizeOfRawData, &bytesWritten)) {
            outError = "ManualMap: Failed to write section '" +
                       std::string(reinterpret_cast<char*>(section->Name), 8) +
                       "'. Error: " + std::to_string(GetLastError());
            VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            return false;
        }
    }
    log.Write("[mapper] step 6 done");

    // Step 7: Process relocations
    // Use the same RVA > file offset helper defined below in step 8
    {
        auto RvaToRaw = [&](uint32_t rva) -> uint8_t* {
            if (rva == 0) return nullptr;
            IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
                if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->Misc.VirtualSize) {
                    return dllBuffer.data() + rva - sec->VirtualAddress + sec->PointerToRawData;
                }
            }
            if (rva < headersSize)
                return dllBuffer.data() + rva;
            return nullptr;
        };

        if (delta != 0) {
            uint32_t relocRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
            uint32_t relocSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

            if (relocRva == 0 || relocSize == 0) {
                outError = "ManualMap: DLL cannot be relocated (no .reloc section), and preferred base (" +
                           std::to_string(preferredBase) + ") is occupied.";
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }

            uint8_t* relocData = RvaToRaw(relocRva);
            if (!relocData) {
                outError = "ManualMap: Failed to locate reloc data at RVA 0x" + std::to_string(relocRva);
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }
            uint8_t* relocEnd = relocData + relocSize;

            while (relocData < relocEnd) {
                IMAGE_BASE_RELOCATION* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(relocData);
                if (block->VirtualAddress == 0 || block->SizeOfBlock == 0) break;

                uint32_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* entry = reinterpret_cast<WORD*>(block + 1);

                for (uint32_t j = 0; j < count; ++j) {
                    if (entry[j] == 0) continue;

                    uint8_t type = (entry[j] >> 12) & 0xF;
                    uint16_t offset = entry[j] & 0xFFF;

                    if (type == IMAGE_REL_BASED_DIR64) {
                        uint64_t fixAddr = reinterpret_cast<uint64_t>(remoteBase) + block->VirtualAddress + offset;
                        uint64_t originalValue = 0;

                        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(fixAddr),
                                              &originalValue, sizeof(originalValue), &bytesWritten)) {
                            originalValue += delta;
                            WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(fixAddr),
                                               &originalValue, sizeof(originalValue), &bytesWritten);
                        }
                    }
                }

                relocData += block->SizeOfBlock;
            }
        }
    }

    // Step 8: Resolve imports 
    // All PE data is read from the LOCAL dllBuffer. RVAs must be converted
    // to file offsets via the section table, because section alignment
    // (0x1000) typically differs from file alignment (0x200).
    auto RvaToFileOffset = [&](uint32_t rva) -> uint8_t* {
        if (rva == 0) return nullptr;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
            if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->Misc.VirtualSize) {
                return dllBuffer.data() + rva - sec->VirtualAddress + sec->PointerToRawData;
            }
        }
        // RVA may be in the header area (before first section)
        if (rva < headersSize)
            return dllBuffer.data() + rva;
        return nullptr;
    };

    uint32_t importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    uint32_t importSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;

    if (importRva != 0 && importSize != 0) {
        IMAGE_IMPORT_DESCRIPTOR* importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(RvaToFileOffset(importRva));
        if (!importDesc) {
            outError = "ManualMap: Failed to locate import descriptor at RVA 0x" + std::to_string(importRva);
            VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
            return false;
        }

        for (; importDesc->Name != 0; ++importDesc) {
            const char* dllName = reinterpret_cast<const char*>(RvaToFileOffset(importDesc->Name));
            if (!dllName) {
                outError = "ManualMap: Invalid import DLL name RVA";
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }
            std::string dllStr(dllName);
            std::wstring wDllName(dllStr.begin(), dllStr.end());

            // try to load the DLL locally for import resolution.
            // system DLLs (kernel32, user32, etc.) are available everywhere.
            // game-specific DLLs (jvm.dll, lwjgl.dll) need to be found in the target.
            HMODULE hLocal = LoadLibraryA(dllStr.c_str());

            if (!hLocal) {
                // Not in our process, find it via target process snapshot
                HANDLE hSnap2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
                if (hSnap2 != INVALID_HANDLE_VALUE) {
                    MODULEENTRY32W me2 = { sizeof(me2) };
                    if (Module32FirstW(hSnap2, &me2)) {
                        do {
                            if (_wcsicmp(me2.szModule, wDllName.c_str()) == 0) {
                                hLocal = LoadLibraryW(me2.szExePath);
                                break;
                            }
                        } while (Module32NextW(hSnap2, &me2));
                    }
                    CloseHandle(hSnap2);
                }
            }

            if (!hLocal) {
                outError = "ManualMap: Failed to load '" + dllStr + "' for import resolution.";
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }

            // Use OriginalFirstThunk (INT) for lookup; FirstThunk (IAT) gets patched
            uint64_t lookupRva = importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk;
            IMAGE_THUNK_DATA64* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(RvaToFileOffset(lookupRva));
            if (!thunk) {
                outError = "ManualMap: Failed to locate thunk data for '" + dllStr + "'";
                FreeLibrary(hLocal);
                VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                return false;
            }

            // Get the module's base in the TARGET process for address rebasing.
            // System DLLs (kernel32, user32...) have the same base across processes,
            // but game DLLs (jvm.dll, lwjgl.dll) dont
            // Some system DLLs (WINHTTP, etc.) may not be loaded in the target yet
            // we can still use local addresses since they'll be loaded at the same base.
            std::string modFindError;
            uint64_t remoteModuleBase = GetRemoteModuleBase(hProcess, wDllName.c_str(), modFindError);
            bool isSystemDll = (remoteModuleBase == 0);

            uint64_t localModuleBase = reinterpret_cast<uint64_t>(hLocal);

            // If DLL not in target but IS a system DLL, use local base directly
            // (system DLLs map at same address across all processes on the session)
            if (isSystemDll) {
                remoteModuleBase = localModuleBase;
            }

            for (int idx = 0; thunk[idx].u1.AddressOfData != 0; ++idx) {
                uint64_t localFuncAddr = 0;

                if (thunk[idx].u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                    uint64_t ordinal = thunk[idx].u1.Ordinal & ~IMAGE_ORDINAL_FLAG64;
                    localFuncAddr = reinterpret_cast<uint64_t>(GetProcAddress(hLocal, reinterpret_cast<const char*>(ordinal)));
                }
                else {
                    IMAGE_IMPORT_BY_NAME* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        RvaToFileOffset(static_cast<uint32_t>(thunk[idx].u1.AddressOfData)));
                    if (!importByName) {
                        outError = "ManualMap: Failed to locate import by name for '" + dllStr + "' idx " + std::to_string(idx);
                        FreeLibrary(hLocal);
                        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                        return false;
                    }
                    localFuncAddr = reinterpret_cast<uint64_t>(GetProcAddress(hLocal, importByName->Name));
                }

                if (localFuncAddr == 0) {
                    outError = "ManualMap: Failed to resolve import from '" + dllStr + "' (function index " +
                               std::to_string(idx) + ").";
                    FreeLibrary(hLocal);
                    VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
                    return false;
                }

                // Rebase: compute the function's RVA from our local copy and
                // add the target process's module base to get the correct remote address.
                // Works for both system DLLs (localBase == remoteBase) and game DLLs.
                uint64_t funcRva = localFuncAddr - localModuleBase;
                uint64_t remoteFuncAddr = remoteModuleBase + funcRva;

                // Write the rebased address into the IAT in the target process
                uint64_t iatAddr = reinterpret_cast<uint64_t>(remoteBase) + importDesc->FirstThunk + idx * sizeof(uint64_t);
                WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(iatAddr), &remoteFuncAddr, sizeof(remoteFuncAddr), &bytesWritten);
            }

            FreeLibrary(hLocal);
        }
    }

    // Step 9: Set final memory protection per section
    section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        DWORD prot = PAGE_READWRITE;
        DWORD chars = section->Characteristics;

        if (chars & IMAGE_SCN_MEM_EXECUTE) {
            prot = (chars & IMAGE_SCN_MEM_WRITE) ? PAGE_EXECUTE_READWRITE :
                   (chars & IMAGE_SCN_MEM_READ)  ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
        }
        else if (chars & IMAGE_SCN_MEM_READ) {
            prot = (chars & IMAGE_SCN_MEM_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        }
        else if (chars & IMAGE_SCN_MEM_WRITE) {
            prot = PAGE_READWRITE;
        }

        if (section->Misc.VirtualSize > 0) {
            LPVOID secAddr = reinterpret_cast<LPVOID>(reinterpret_cast<uint64_t>(remoteBase) + section->VirtualAddress);
            DWORD oldProt = 0;
            VirtualProtectEx(hProcess, secAddr, section->Misc.VirtualSize, prot, &oldProt);
        }
    }

    // Step 10: Build shellcode to call DllMain
    uint64_t entryRva = nt->OptionalHeader.AddressOfEntryPoint;
    if (entryRva == 0) {
        outError = "ManualMap: DLL has no entry point (AddressOfEntryPoint is 0).";
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    uint64_t entryPoint = reinterpret_cast<uint64_t>(remoteBase) + entryRva;

    // Allocate shellcode buffer in target
    Shellcode64 shellcode;
    shellcode.imageBase = reinterpret_cast<uint64_t>(remoteBase);
    shellcode.entryPoint = entryPoint;

    size_t shellcodeSize = sizeof(Shellcode64);
    LPVOID shellcodeBase = VirtualAllocEx(hProcess, NULL, shellcodeSize,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // FIX: RW first, protect later
    if (!shellcodeBase) {
        outError = "ManualMap: Failed to allocate shellcode memory. Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    if (!WriteProcessMemory(hProcess, shellcodeBase, &shellcode, shellcodeSize, &bytesWritten)) {
        outError = "ManualMap: Failed to write shellcode. Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, shellcodeBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProt = 0;
    VirtualProtectEx(hProcess, shellcodeBase, shellcodeSize, PAGE_EXECUTE_READ, &oldProt);

    FlushInstructionCache(hProcess, shellcodeBase, shellcodeSize);

    //  Step 11: Create remote thread from which DLLMain can finally be executed
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
                                        reinterpret_cast<LPTHREAD_START_ROUTINE>(shellcodeBase),
                                        NULL, 0, NULL);
    if (!hThread) {
        outError = "ManualMap: Failed to create remote shellcode thread. Error: " + std::to_string(GetLastError());
        VirtualFreeEx(hProcess, shellcodeBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    DWORD waitResult = WaitForSingleObject(hThread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Free shellcode memory (it already ran)
    VirtualFreeEx(hProcess, shellcodeBase, 0, MEM_RELEASE);

    if (waitResult == WAIT_TIMEOUT) {
        outError = "ManualMap: Shellcode thread timed out after 15 seconds (DllMain may be hung).";
        // DLL is still mapped, return success since it might have partially initialized
        return true;
    }

    if (exitCode == 0) {
        outError = "ManualMap: DllMain returned FALSE. The DLL may have failed initialization.\n\n"
                   "This is usually caused by:\n"
                   " - Missing dependencies in the target process\n"
                   " - The DLL is already loaded (conflict)\n"
                   " - Incompatible Minecraft version\n\n"
                   "Exit code: " + std::to_string(exitCode);
        return false;
    }

    // Check for crash (exit code >= 0x80000000 means exception)
    if (exitCode >= 0x80000000) {
        char hexCode[32];
        sprintf_s(hexCode, "0x%08X", exitCode);
        outError = std::string("ManualMap: DllMain crashed with exception code ") + hexCode + ".\n\n"
                   "This could be due to:\n"
                   " - Missing or incorrect import resolution\n"
                   " - Incompatible Minecraft/Java version\n"
                   " - The DLL is corrupt\n\n"
                   "Falling back to LoadLibrary method...";
        return false;
    }

    // the manual mapping worked (report success)
    return true;
}
