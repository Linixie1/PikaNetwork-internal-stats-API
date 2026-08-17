#include "screen.h"
#include <string>
#include <windows.h>
#include <tlhelp32.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "process/process.h"
#include "folder/folder.h"
#include "base.h"
#include "../util/log.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <set>
#include "../resource.h"

#define WINDOW_WIDTH 700
#define WINDOW_HEIGHT 400

static std::string g_oldDllPath = "";
static std::atomic<bool> g_injecting{ false };
static std::atomic<bool> g_injectFinished{ false };
static std::atomic<bool> g_injectSuccess{ false };
static std::string g_injectError;
static std::chrono::steady_clock::time_point g_injectStartTime;

static std::set<DWORD> g_injectedPids;
static std::mutex g_injectedPidsMutex;

static bool IsMinecraftFullyReady(DWORD processId, HWND& outHwnd)
{
    outHwnd = nullptr;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    bool hasOpengl = false;
    bool hasJvm = false;
    bool hasAudio = false; 

    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"opengl32.dll") == 0) hasOpengl = true;
            if (_wcsicmp(me.szModule, L"jvm.dll") == 0) hasJvm = true;
            if (_wcsicmp(me.szModule, L"soft_oal.dll") == 0 || 
                _wcsicmp(me.szModule, L"OpenAL.dll") == 0 || 
                _wcsicmp(me.szModule, L"OpenAL64.dll") == 0) {
                hasAudio = true;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    if (!hasOpengl || !hasJvm || !hasAudio) return false;

    HWND foundHwnd = nullptr;
    struct EnumContext {
        DWORD targetPid;
        HWND* outHwnd;
    };
    EnumContext ctx{ processId, &foundHwnd };

    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        EnumContext* ec = (EnumContext*)lparam;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ec->targetPid) {
            if (IsWindowVisible(hwnd) && GetWindowTextLength(hwnd) > 0) {
                char className[256] = {};
                GetClassNameA(hwnd, className, sizeof(className));
                std::string cls(className);

                if (cls.find("GLFW") != std::string::npos || cls.find("LWJGL") != std::string::npos || cls.find("SunAwtFrame") != std::string::npos) {
                    RECT rc = {};
                    GetClientRect(hwnd, &rc);
                    if ((rc.right - rc.left) > 120 && (rc.bottom - rc.top) > 120) {
                        *(ec->outHwnd) = hwnd;
                        return FALSE;
                    }
                }
            }
        }
        return TRUE;
    }, (LPARAM)&ctx);

    if (!foundHwnd) return false;

    DWORD_PTR dwResult = 0;
    LRESULT lr = SendMessageTimeoutA(
        foundHwnd,
        WM_NULL,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        800,
        &dwResult
    );

    if (lr == 0) return false; 

    outHwnd = foundHwnd;
    return true;
}

static std::string GetMinecraftClientType(DWORD processId, HWND hwnd)
{
    std::string clientType = "vanilla";

    if (hwnd && IsWindow(hwnd)) {
        char title[512] = {};
        GetWindowTextA(hwnd, title, sizeof(title));
        std::string lowerTitle = title;
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), ::tolower);

        if (lowerTitle.find("lunar") != std::string::npos) return "lunar";
        if (lowerTitle.find("badlion") != std::string::npos) return "badlion";
        if (lowerTitle.find("laby") != std::string::npos) return "labymod";
        if (lowerTitle.find("forge") != std::string::npos) return "forge";
        if (lowerTitle.find("feather") != std::string::npos) return "feather";
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        if (Module32FirstW(hSnap, &me)) {
            do {
                std::wstring modName(me.szModule);
                std::transform(modName.begin(), modName.end(), modName.begin(), ::towlower);
                if (modName.find(L"lunar") != std::wstring::npos) { clientType = "lunar"; break; }
                if (modName.find(L"badlion") != std::wstring::npos) { clientType = "badlion"; break; }
                if (modName.find(L"labymod") != std::wstring::npos) { clientType = "labymod"; break; }
            } while (Module32NextW(hSnap, &me));
        }
        CloseHandle(hSnap);
    }
    return clientType;
}

static void SehTranslator(unsigned int code, EXCEPTION_POINTERS*)
{
    char buf[128];
    sprintf_s(buf, "SEH exception 0x%08X", code);
    throw std::runtime_error(buf);
}

static void InjectionThreadProc(DWORD processId, const std::string& dllPath)
{
    Logger& log = Logger::GetInstance();
    log.Write("[loader] injection thread started for PID " + std::to_string(processId));
    _set_se_translator(SehTranslator);

    try
    {
        std::string injectError;
        const ULONGLONG POLL_INTERVAL_MS = 250;
        const ULONGLONG TIMEOUT_MS = 120000; 
        ULONGLONG startThreadTime = GetTickCount64();
        bool modulesFound = false;
        ULONGLONG moduleFoundTime = 0;
        ULONGLONG expectedStabilizationTime = 2000;
        HWND gameHwnd = nullptr;
        int responsiveConsecutivePolls = 0;

        for (;;)
        {
            HANDLE hCheck = OpenProcess(SYNCHRONIZE, FALSE, processId);
            if (!hCheck)
            {
                g_injectError = "minecraft process exited before injection could start.";
                g_injectSuccess = false;
                g_injectFinished = true;
                return;
            }
            DWORD waitResult = WaitForSingleObject(hCheck, 0); 
            CloseHandle(hCheck);
            if (waitResult == WAIT_OBJECT_0)
            {
                g_injectError = "minecraft process exited before injection could start.";
                g_injectSuccess = false;
                g_injectFinished = true;
                return;
            }

            if ((GetTickCount64() - startThreadTime) >= TIMEOUT_MS) {
                g_injectError = "injection timed out waiting for game window to stabilize.";
                g_injectSuccess = false;
                g_injectFinished = true;
                return;
            }

            if (!modulesFound) 
            {
                if (IsMinecraftFullyReady(processId, gameHwnd)) 
                {
                    responsiveConsecutivePolls++;
                    if (responsiveConsecutivePolls >= 4) {
                        modulesFound = true;
                        moduleFoundTime = GetTickCount64();
                        
                        std::string clientType = GetMinecraftClientType(processId, gameHwnd);
                        ULONGLONG processUptime = ProcessManager::GetProcessUptimeMs(processId);

                        if (processUptime > 45000) {
                            expectedStabilizationTime = 500;
                        } else if (clientType == "lunar") {
                            expectedStabilizationTime = 10000;
                        } else if (clientType == "badlion" || clientType == "labymod") {
                            expectedStabilizationTime = 6000;
                        } else if (clientType == "forge") {
                            expectedStabilizationTime = 5000;
                        } else {
                            expectedStabilizationTime = 3000; 
                        }

                        log.Write("[loader] " + clientType + " client detected. entering " + std::to_string(expectedStabilizationTime) + "ms post-load phase...");
                    }
                }
                else {
                    responsiveConsecutivePolls = 0;
                }
            }

            if (modulesFound) 
            {
                ULONGLONG timeSinceFound = GetTickCount64() - moduleFoundTime;
                ULONGLONG processUptime = ProcessManager::GetProcessUptimeMs(processId);

                DWORD_PTR dwResult = 0;
                bool isPumping = true;
                if (gameHwnd && IsWindow(gameHwnd)) {
                    LRESULT lr = SendMessageTimeoutA(gameHwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, &dwResult);
                    if (lr == 0) isPumping = false;
                }

                if (isPumping && (processUptime > 45000 || timeSinceFound >= expectedStabilizationTime)) 
                {
                    log.Write("[loader] game is fully at main menu. safe to inject.");
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }

        log.Write("[loader] injecting via LoadLibrary...");
        if (ProcessManager::InjectDLL(processId, dllPath.c_str(), injectError))
        {
            g_injectSuccess = true;
            g_injectFinished = true;
            return;
        }

        g_injectError = injectError;
        g_injectSuccess = false;
        g_injectFinished = true;
    }
    catch (const std::exception& e)
    {
        g_injectError = std::string("injection crashed: ") + e.what();
        g_injectSuccess = false;
        g_injectFinished = true;
    }
    catch (...)
    {
        g_injectError = "injection crashed with unknown exception.";
        g_injectSuccess = false;
        g_injectFinished = true;
    }
}

static bool TryAutoInject(std::vector<ProcessManager::WindowInfo>& processes)
{
    if (g_injecting) return false;
    std::string dllPath = ProcessManager::GetDllPath();
    if (dllPath.empty())
        dllPath = FolderManager::GetDllPath();
    if (dllPath.empty()) return false;

    for (auto& proc : processes)
    {
        {
            std::lock_guard<std::mutex> lock(g_injectedPidsMutex);
            if (g_injectedPids.count(proc.processId))
                continue;
        }
        
        Logger::GetInstance().Write("[loader] auto-inject: new target PID " + std::to_string(proc.processId) + " (" + proc.processName + ")");
        ProcessManager::ExtractEmbeddedPayloads();
        std::string freshPath = ProcessManager::GetDllPath();
        if (freshPath.empty()) freshPath = dllPath;

        g_injecting = true;
        g_injectFinished = false;
        g_injectSuccess = false;
        g_injectError.clear();
        g_injectStartTime = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(g_injectedPidsMutex);
            g_injectedPids.insert(proc.processId);
        }

        std::thread(InjectionThreadProc, proc.processId, freshPath).detach();
        return true;
    }
    return false;
}

static void CleanupStalePids()
{
    std::lock_guard<std::mutex> lock(g_injectedPidsMutex);
    std::vector<DWORD> toRemove;
    for (DWORD pid : g_injectedPids)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h || h == INVALID_HANDLE_VALUE)
        {
            toRemove.push_back(pid);
        }
        else
        {
            DWORD code = 0;
            GetExitCodeProcess(h, &code);
            CloseHandle(h);
            if (code != STILL_ACTIVE)
                toRemove.push_back(pid);
        }
    }
    for (DWORD pid : toRemove)
        g_injectedPids.erase(pid);
}

void Screen::SetupStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildRounding = 8.0f;
    style.ChildBorderSize = 1.0f;
    style.FramePadding = ImVec2(12.0f, 6.0f);
    style.FrameRounding = 6.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 10.0f);

    style.Colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.90f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.60f, 0.50f, 0.65f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.04f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_ChildBg]               = ImVec4(0.08f, 0.05f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_Border]                = ImVec4(0.50f, 0.30f, 0.60f, 0.30f);
    style.Colors[ImGuiCol_FrameBg]               = ImVec4(0.15f, 0.10f, 0.25f, 0.80f);
    style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.25f, 0.15f, 0.40f, 0.80f);
    style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(0.35f, 0.20f, 0.50f, 1.00f);
    style.Colors[ImGuiCol_Button]                = ImVec4(0.25f, 0.15f, 0.40f, 0.90f);
    style.Colors[ImGuiCol_ButtonHovered]         = ImVec4(0.45f, 0.25f, 0.65f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive]          = ImVec4(0.60f, 0.35f, 0.85f, 1.00f);
}

std::atomic<bool> dllUpdating(false);
std::atomic<bool> injectorUpdating(false);
std::atomic<bool> updateFinished(false);
std::string updateStatus = "";

static void UpdateDllThread()
{
    dllUpdating = true;
    updateStatus = "updating dll...";
    if (BaseUtils::UpdateDll(g_oldDllPath)) updateStatus = "dll updated yay!";
    else updateStatus = "dll update failed rip!";
    dllUpdating = false;
}

static void UpdateInjectorThread()
{
    injectorUpdating = true;
    updateStatus = "updating loader...";
    if (BaseUtils::UpdateInjector()) {
        updateStatus = "loader updated!";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    } else {
        updateStatus = "loader update failed!";
    }
    injectorUpdating = false;
}

static void AnimateUpdateStatus()
{
    static int dotCount = 3;
    static int lastUpdateTime = 0;
    int currentTime = static_cast<int>(std::chrono::system_clock::now().time_since_epoch().count() / 1000000000);
    
    if (currentTime - lastUpdateTime >= 1) { 
        dotCount = (dotCount + 1) % 4; 
        lastUpdateTime = currentTime; 
    }
    
    std::string dotStr(dotCount, '.');
    if (dllUpdating && injectorUpdating) updateStatus = "updating dll and loader" + dotStr;
    else if (dllUpdating) updateStatus = "updating dll" + dotStr;
    else if (injectorUpdating) updateStatus = "updating loader" + dotStr;
}

static ImVec4 HSV(float h, float s, float v, float a = 1.f)
{
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(fmodf(h, 1.f), s, v, r, g, b);
    return ImVec4(r, g, b, a);
}

static ImU32 HSVU32(float h, float s, float v, float a = 1.f)
{
    ImVec4 c = HSV(h, s, v, a);
    return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), (int)(c.w * 255));
}

struct CutieSparkle { float x, y, vy, life, maxLife, sz, hue; };
static CutieSparkle g_sparks[60];
static bool g_sparksInit = false;

bool Screen::Render()
{
    if (g_injecting && g_injectFinished)
    {
        g_injecting = false;
        if (g_injectSuccess)
        {
            Logger::GetInstance().Write("[loader] injection successful");
        }
        else
        {
            std::string errMsg = "injection failed kinda cringe.\n\n" + g_injectError;
            Logger::GetInstance().Write("[loader] injection failed: " + g_injectError);
            MessageBoxA(nullptr, errMsg.c_str(), "cutie - Error", MB_OK | MB_ICONERROR);
        }
    }

    if (g_injecting && !g_injectFinished)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_injectStartTime).count();
        if (elapsed > 120) 
        {
            g_injecting = false;
            Logger::GetInstance().Write("[loader] injection timed out");
            MessageBoxA(nullptr, "injection timed out.\n\nmake sure defender isnt eating the loader.",
                "cutie - Timeout", MB_OK | MB_ICONERROR);
        }
    }

    static bool update = BaseUtils::IsInjectorUpdated();
    static bool dllUpdated = false;
    static std::vector<ProcessManager::WindowInfo> processes;
    static int selectedProcess = 0;
    
    static auto lastScan = std::chrono::steady_clock::now();
    static bool firstScan = true;
    auto nowScan = std::chrono::steady_clock::now();

    if (firstScan || std::chrono::duration_cast<std::chrono::milliseconds>(nowScan - lastScan).count() >= 2000)
    {
        firstScan = false;
        lastScan = nowScan;

        ProcessManager::GetMinecraftProcesses(processes);
        if (selectedProcess >= (int)processes.size()) selectedProcess = 0;
        if (!processes.empty() && !g_injecting)
            TryAutoInject(processes);
        CleanupStalePids();
    }

    std::string dllVersion = FolderManager::GetVersionStringDll();
    std::string injectorVersion = INJECTOR_VERSION;
    bool minecraft_running = processes.size() > 0;
    bool multiple_minecraft_instances = processes.size() > 1;

    float t    = (float)ImGui::GetTime();
    float dt   = ImGui::GetIO().DeltaTime;
    float winW = ImGui::GetIO().DisplaySize.x;
    float winH = ImGui::GetIO().DisplaySize.y;
    float baseH = 0.82f + 0.08f * sinf(t * 0.8f);

    if (!g_sparksInit)
    {
        srand(1337);
        for (auto& s : g_sparks)
        {
            s.x = (float)(rand() % (int)(winW > 0 ? winW : 700));
            s.y = (float)(rand() % (int)(winH > 0 ? winH : 400));
            s.vy = -(8.f + rand() % 25);
            s.life = (float)(rand() % 100) / 100.f * 3.f;
            s.maxLife = 3.0f + (float)(rand() % 200) / 100.f;
            s.sz = 1.0f + (float)(rand() % 20) / 10.f;
            s.hue = 0.75f + (float)(rand() % 20) / 100.f; 
        }
        g_sparksInit = true;
    }

    for (auto& s : g_sparks)
    {
        s.life += dt; 
        s.y += s.vy * dt;
        s.x += sinf(t * 2.0f + s.life) * 0.5f;
        if (s.life >= s.maxLife || s.y < -10.f)
        {
            s.x = (float)(rand() % (int)(winW > 0 ? winW : 700));
            s.y = winH + 5.f;
            s.vy = -(8.f + rand() % 25);
            s.life = 0.f;
            s.maxLife = 3.0f + (float)(rand() % 200) / 100.f;
            s.sz = 1.0f + (float)(rand() % 20) / 10.f;
            s.hue = 0.75f + (float)(rand() % 20) / 100.f;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Cutie Loader", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);

    ImVec2 wp = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + winW, wp.y + winH), IM_COL32(12, 8, 18, 255));

    float gp = 0.5f + 0.5f * sinf(t * 0.7f);
    dl->AddCircleFilled(ImVec2(wp.x + winW * 0.5f, wp.y + winH * 0.4f), winH * 0.65f, IM_COL32(70, 20, 90, (int)(15 + 10 * gp)), 64);

    for (auto& s : g_sparks)
    {
        float alpha = sinf(s.life / s.maxLife * 3.14159f);
        ImU32 sc = HSVU32(s.hue, 0.60f, 1.f, alpha * 0.8f);
        dl->AddCircleFilled(ImVec2(wp.x + s.x, wp.y + s.y), s.sz * 2.0f, HSVU32(s.hue, 0.6f, 1.f, alpha * 0.2f), 6);
        dl->AddCircleFilled(ImVec2(wp.x + s.x, wp.y + s.y), s.sz, sc, 6);
    }

    const float cW = 360.f;
    const float cX = (winW - cW) * 0.5f;
    const float cY = winH * 0.09f;
    ImGui::SetCursorPos(ImVec2(cX, cY));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("content", ImVec2(cW, winH - cY * 1.8f), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImVec2 cp = ImGui::GetWindowPos();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImFont* fnt = ImGui::GetFont();
    float fntSz = ImGui::GetFontSize();
    float titleSz = fntSz * 2.8f;
    float pulse = 0.5f + 0.5f * sinf(t * 2.2f);
    float cursor = 15.f;

    const char* title = "cutie";
    float titleTotalW = fnt->CalcTextSizeA(titleSz, FLT_MAX, 0.f, title, nullptr, nullptr).x;
    float tx = cp.x + (cW - titleTotalW) * 0.5f;
    float ty = cp.y + cursor;

    for (int g = 4; g >= 1; g--)
    {
        float spread = (float)g * 1.5f;
        float ga = (0.08f + 0.04f * pulse) / (float)g;
        ImU32 gc = HSVU32(baseH, 0.7f, 1.f, ga);
        for (float ddx = -spread; ddx <= spread; ddx += spread)
            for (float ddy = -spread; ddy <= spread; ddy += spread)
                cdl->AddText(fnt, titleSz, ImVec2(tx + ddx, ty + ddy), gc, title);
    }

    float cx2 = tx;
    for (int i = 0; title[i]; i++)
    {
        char ch[2] = { title[i], 0 };
        float cw2 = fnt->CalcTextSizeA(titleSz, FLT_MAX, 0.f, ch, nullptr, nullptr).x;
        cdl->AddText(fnt, titleSz, ImVec2(cx2, ty), HSVU32(fmodf(baseH + (float)i * 0.05f, 1.f), 0.55f, 0.95f, 1.f), ch);
        cx2 += cw2;
    }
    cursor += fnt->CalcTextSizeA(titleSz, FLT_MAX, 0.f, title, nullptr, nullptr).y + 2.f;

    const char* sub = "the cutest injector";
    float subSz = fntSz * 0.85f;
    float subW = fnt->CalcTextSizeA(subSz, FLT_MAX, 0.f, sub, nullptr, nullptr).x;
    cdl->AddText(fnt, subSz, ImVec2(cp.x + (cW - subW) * 0.5f, cp.y + cursor), HSVU32(baseH, 0.4f, 0.8f, 0.5f + 0.2f * sinf(t * 1.5f)), sub);
    cursor += fnt->CalcTextSizeA(subSz, FLT_MAX, 0.f, sub, nullptr, nullptr).y + 22.f;

    {
        float sepPulse = 0.5f + 0.5f * sinf(t * 1.6f);
        float sx0 = cp.x + 20.f, sx1 = cp.x + cW - 20.f, sy = cp.y + cursor;
        int segs = 40;
        for (int i = 0; i < segs; i++)
        {
            float f0 = (float)i / segs, f1 = (float)(i + 1) / segs;
            float x0 = sx0 + f0 * (sx1 - sx0), x1 = sx0 + f1 * (sx1 - sx0);
            float dist = fabsf((f0 + f1) * 0.5f - 0.5f) * 2.f;
            cdl->AddLine(ImVec2(x0, sy), ImVec2(x1, sy), HSVU32(baseH, 0.6f, 1.f, (1.f - dist) * (0.35f + 0.25f * sepPulse)), 2.0f);
        }
        cursor += 20.f;
    }

    {
        float bdH = 28.f, gap = 12.f;
        float bdW = (cW - gap - 40.f) * 0.5f;
        float bd1X = cp.x + 20.f, bd2X = bd1X + bdW + gap, bdY = cp.y + cursor;
        float bdPulse = 0.5f + 0.5f * sinf(t * 2.0f);

        cdl->AddRectFilled(ImVec2(bd1X, bdY), ImVec2(bd1X + bdW, bdY + bdH), IM_COL32(25, 15, 35, 180), 6.f);
        cdl->AddRect(ImVec2(bd1X, bdY), ImVec2(bd1X + bdW, bdY + bdH), HSVU32(baseH, 0.5f, 0.9f, 0.3f + 0.2f * bdPulse), 6.f, 0, 1.5f);
        cdl->AddRectFilled(ImVec2(bd2X, bdY), ImVec2(bd2X + bdW, bdY + bdH), IM_COL32(25, 15, 35, 180), 6.f);
        cdl->AddRect(ImVec2(bd2X, bdY), ImVec2(bd2X + bdW, bdY + bdH), HSVU32(fmodf(baseH + 0.05f, 1.f), 0.5f, 0.9f, 0.3f + 0.2f * bdPulse), 6.f, 0, 1.5f);

        float lblSz = fntSz * 0.85f;
        float lblH = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, "X", nullptr, nullptr).y;
        std::string dllLbl = "dll  " + dllVersion;
        std::string ldrLbl = "ldr  " + injectorVersion;
        float dlW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, dllLbl.c_str(), nullptr, nullptr).x;
        float ldW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, ldrLbl.c_str(), nullptr, nullptr).x;
        cdl->AddText(fnt, lblSz, ImVec2(bd1X + (bdW - dlW) * 0.5f, bdY + (bdH - lblH) * 0.5f), HSVU32(baseH, 0.2f, 0.95f, 1.f), dllLbl.c_str());
        cdl->AddText(fnt, lblSz, ImVec2(bd2X + (bdW - ldW) * 0.5f, bdY + (bdH - lblH) * 0.5f), HSVU32(fmodf(baseH + 0.05f, 1.f), 0.2f, 0.95f, 1.f), ldrLbl.c_str());
        cursor += bdH + 25.f;
    }

    {
        std::string tgt = processes.size() > 0 ? processes[selectedProcess].processName : "waiting for mc...";
        float tgtAlpha = minecraft_running ? 1.f : 0.5f;
        float lblSz = fntSz * 0.9f;
        const char* lbl = "target  ";
        float lblW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, lbl, nullptr, nullptr).x;
        float tgtW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, tgt.c_str(), nullptr, nullptr).x;
        float lblH = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, "X", nullptr, nullptr).y;
        float startX = cp.x + (cW - lblW - tgtW) * 0.5f;

        cdl->AddText(fnt, lblSz, ImVec2(startX, cp.y + cursor), HSVU32(baseH, 0.4f, 0.8f, tgtAlpha * 0.7f), lbl);
        cdl->AddText(fnt, lblSz, ImVec2(startX + lblW, cp.y + cursor), HSVU32(baseH, 0.2f, 0.95f, tgtAlpha), tgt.c_str());
        cursor += lblH + 25.f;
    }

    {
        float btnH = 46.f;
        float injPulse = 0.5f + 0.5f * sinf(t * 3.0f);
        float btn1X = cp.x + 20.f, btn1W = cW - 40.f;

        for (int g = 4; g >= 1; g--)
        {
            float exp2 = (float)g * 2.0f;
            float ga = (0.05f + 0.05f * injPulse) / (float)g;
            ImU32 gc = HSVU32(baseH, 0.7f, 1.f, ga);
            cdl->AddRectFilled(ImVec2(btn1X - exp2, cp.y + cursor - exp2), ImVec2(btn1X + btn1W + exp2, cp.y + cursor + btnH + exp2), gc, 8.f + exp2);
        }

        if (g_injecting)
        {
            const char* injText = "injecting...";
            float injW = fnt->CalcTextSizeA(fntSz * 1.1f, FLT_MAX, 0.f, injText, nullptr, nullptr).x;
            cdl->AddText(fnt, fntSz * 1.1f, ImVec2(cp.x + (cW - injW) * 0.5f, cp.y + cursor + (btnH - fntSz * 1.1f) * 0.5f), HSVU32(baseH, 0.4f, 0.95f, 1.f), injText);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, HSV(baseH, 0.6f, 0.4f + 0.10f * injPulse, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HSV(baseH, 0.5f, 0.6f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, HSV(baseH, 0.4f, 0.8f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
            ImGui::SetCursorPos(ImVec2(20.f, cursor));

            if (ImGui::Button(minecraft_running ? "inject" : "watching...", ImVec2(btn1W, btnH)))
            {
                if (minecraft_running)
                {
                    std::string dllPath = ProcessManager::GetDllPath();
                    if (dllPath.empty()) dllPath = FolderManager::GetDllPath();
                    if (dllPath.empty())
                    {
                        Logger::GetInstance().Write("[loader] DLL not found");
                        MessageBoxA(nullptr, "cutie.dll not found.\nre-extract the loader from its archive.", "cutie - Error", MB_OK | MB_ICONERROR);
                    }
                    else
                    {
                        ProcessManager::ExtractEmbeddedPayloads();
                        std::string freshPath = ProcessManager::GetDllPath();
                        if (freshPath.empty()) freshPath = dllPath;

                        g_injecting = true;
                        g_injectFinished = false;
                        g_injectSuccess = false;
                        g_injectError.clear();
                        g_injectStartTime = std::chrono::steady_clock::now();

                        {
                            std::lock_guard<std::mutex> lock(g_injectedPidsMutex);
                            g_injectedPids.insert(processes[selectedProcess].processId);
                        }

                        std::thread(InjectionThreadProc, processes[selectedProcess].processId, freshPath).detach();
                    }
                }
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
        }
        cursor += btnH + 15.f;
    }

    if (multiple_minecraft_instances)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.10f, 0.25f, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.15f, 0.35f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.20f, 0.45f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 4.f));
        ImGui::SetCursorPos(ImVec2(20.f, cursor));

        if (ImGui::Button("switch instance", ImVec2(cW - 40.f, 0.f)))
        {
            selectedProcess++;
            if (selectedProcess >= (int)processes.size()) selectedProcess = 0;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        cursor += ImGui::GetFrameHeight() + 10.f;
    }

    AnimateUpdateStatus();
    
    if (dllUpdating || injectorUpdating)
    {
        float stSz = fntSz * 0.85f;
        float stW = fnt->CalcTextSizeA(stSz, FLT_MAX, 0.f, updateStatus.c_str(), nullptr, nullptr).x;
        cdl->AddText(fnt, stSz, ImVec2(cp.x + (cW - stW) * 0.5f, cp.y + cursor), HSVU32(baseH, 0.4f, 0.9f, 0.85f), updateStatus.c_str());
    }

    const char* wmText = "discord: linixie.";
    float wmSz = fntSz * 0.8f;
    float wmW = fnt->CalcTextSizeA(wmSz, FLT_MAX, 0.f, wmText, nullptr, nullptr).x;
    float wmX = wp.x + winW - wmW - 12.f;
    float wmY = wp.y + winH - wmSz - 12.f;

    for (int i = 0; i < 3; i++) {
        dl->AddText(fnt, wmSz, ImVec2(wmX - i, wmY - i), HSVU32(baseH, 0.6f, 1.f, 0.15f), wmText);
    }
    dl->AddText(fnt, wmSz, ImVec2(wmX, wmY), HSVU32(fmodf(baseH + 0.05f, 1.f), 0.4f, 0.9f, 0.7f), wmText);

    ImGui::EndChild();
    ImGui::End();

    if (!BaseUtils::IsDllUpdated() && !dllUpdated)
    {
        g_oldDllPath = FolderManager::GetDllPath();
        std::thread(UpdateDllThread).detach();
        dllUpdated = true;
    }
    else dllUpdated = true;

    return true;
}