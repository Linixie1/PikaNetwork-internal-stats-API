#include "screen.h"

#include <string>
#include <windows.h>
#include <sstream>
#include <iomanip>

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

const ULONGLONG MIN_PROCESS_UPTIME_MS = 15000;
        const ULONGLONG POLL_INTERVAL_MS = 500;

        for (;;)
        {
            ULONGLONG uptime = ProcessManager::GetProcessUptimeMs(processId);
            if (uptime == 0)
            {
                log.Write("[loader] could not query process uptime for PID " + std::to_string(processId) + " - proceeding anyway");
                break;
            }

            if (uptime >= MIN_PROCESS_UPTIME_MS)
            {
                log.Write("[loader] process uptime " + std::to_string(uptime) + " ms >= " +
                         std::to_string(MIN_PROCESS_UPTIME_MS) + " ms, proceeding with injection");
                break;
            }

            HANDLE hCheck = OpenProcess(SYNCHRONIZE, FALSE, processId);
            if (!hCheck)
            {
                log.Write("[loader] target process exited during uptime wait, aborting");
                g_injectError = "Minecraft process exited before injection could start.";
                g_injectSuccess = false;
                g_injectFinished = true;
                return;
            }
            DWORD waitResult = WaitForSingleObject(hCheck, POLL_INTERVAL_MS);
            CloseHandle(hCheck);

            if (waitResult == WAIT_OBJECT_0)
            {
                log.Write("[loader] target process exited during uptime wait, aborting");
                g_injectError = "Minecraft process exited before injection could start.";
                g_injectSuccess = false;
                g_injectFinished = true;
                return;
            }

            log.Write("[loader] waiting for process to stabilize... uptime " +
                     std::to_string(uptime) + " ms / " + std::to_string(MIN_PROCESS_UPTIME_MS) + " ms");
        }

        log.Write("[loader] injecting via LoadLibrary...");

        if (ProcessManager::InjectDLL(processId, dllPath.c_str(), injectError))
        {
            log.Write("[loader] injection succeeded");
            g_injectSuccess = true;
            g_injectFinished = true;
            return;
        }

        log.Write("[loader] LoadLibrary injection failed: " + injectError);
        g_injectError = injectError;
        g_injectSuccess = false;
        g_injectFinished = true;
    }
    catch (const std::exception& e)
    {
        log.Write(std::string("[loader] EXCEPTION in injection thread: ") + e.what());
        g_injectError = std::string("Injection crashed: ") + e.what() +
            "\n\nMake sure the loader folder is excluded in Windows Defender.";
        g_injectSuccess = false;
        g_injectFinished = true;
    }
    catch (...)
    {
        log.Write("[loader] UNKNOWN EXCEPTION in injection thread");
        g_injectError = "Injection crashed.\n\n"
            "Make sure the loader folder is excluded in Windows Defender.";
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
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 8.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 6.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(12.0f, 6.0f);
    style.FrameRounding = 6.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.CellPadding = ImVec2(6.0f, 6.0f);
    style.IndentSpacing = 25.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    style.TabBorderSize = 0.0f;
    style.TabMinWidthForCloseButton = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.92f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.50f, 0.65f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.07f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.09f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.10f, 0.18f, 0.96f);
    style.Colors[ImGuiCol_Border]                = ImVec4(0.45f, 0.35f, 0.65f, 0.30f);
    style.Colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.20f);
    style.Colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.12f, 0.22f, 0.80f);
    style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.18f, 0.34f, 0.80f);
    style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(0.30f, 0.24f, 0.46f, 1.00f);
    style.Colors[ImGuiCol_TitleBg]               = ImVec4(0.06f, 0.05f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.08f, 0.18f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.06f, 0.05f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.08f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.06f, 0.05f, 0.10f, 0.54f);
    style.Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.40f, 0.30f, 0.60f, 0.54f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.55f, 0.42f, 0.75f, 0.54f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.65f, 0.50f, 0.85f, 0.54f);
    style.Colors[ImGuiCol_CheckMark]             = ImVec4(0.72f, 0.55f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]            = ImVec4(0.55f, 0.40f, 0.78f, 0.54f);
    style.Colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.70f, 0.55f, 0.90f, 0.54f);
    style.Colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.15f, 0.35f, 0.90f);
    style.Colors[ImGuiCol_ButtonHovered]         = ImVec4(0.45f, 0.32f, 0.72f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive]          = ImVec4(0.58f, 0.42f, 0.88f, 1.00f);
    style.Colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.15f, 0.35f, 0.52f);
    style.Colors[ImGuiCol_HeaderHovered]         = ImVec4(0.35f, 0.25f, 0.55f, 0.52f);
    style.Colors[ImGuiCol_HeaderActive]          = ImVec4(0.45f, 0.32f, 0.68f, 0.52f);
    style.Colors[ImGuiCol_Separator]             = ImVec4(0.45f, 0.35f, 0.65f, 0.25f);
    style.Colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.60f, 0.45f, 0.80f, 0.35f);
    style.Colors[ImGuiCol_SeparatorActive]       = ImVec4(0.72f, 0.55f, 0.92f, 1.00f);
    style.Colors[ImGuiCol_ResizeGrip]            = ImVec4(0.45f, 0.35f, 0.65f, 0.20f);
    style.Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.60f, 0.45f, 0.80f, 0.30f);
    style.Colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.72f, 0.55f, 0.92f, 1.00f);
    style.Colors[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.10f, 0.20f, 0.52f);
    style.Colors[ImGuiCol_TabHovered]            = ImVec4(0.35f, 0.25f, 0.55f, 1.00f);
    style.Colors[ImGuiCol_TabActive]             = ImVec4(0.25f, 0.18f, 0.42f, 1.00f);
    style.Colors[ImGuiCol_TabUnfocused]          = ImVec4(0.10f, 0.08f, 0.18f, 0.52f);
    style.Colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.18f, 0.14f, 0.30f, 1.00f);
    style.Colors[ImGuiCol_PlotLines]             = ImVec4(0.72f, 0.55f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.88f, 0.70f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogram]         = ImVec4(0.72f, 0.55f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.88f, 0.70f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.12f, 0.10f, 0.20f, 0.52f);
    style.Colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.12f, 0.10f, 0.20f, 0.52f);
    style.Colors[ImGuiCol_TableBorderLight]      = ImVec4(0.35f, 0.28f, 0.50f, 0.25f);
    style.Colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);
    style.Colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.45f, 0.32f, 0.72f, 0.40f);
    style.Colors[ImGuiCol_DragDropTarget]        = ImVec4(0.72f, 0.55f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_NavHighlight]          = ImVec4(0.72f, 0.55f, 0.95f, 1.00f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.72f, 0.55f, 0.95f, 0.70f);
    style.Colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.20f, 0.15f, 0.35f, 0.20f);
    style.Colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.20f, 0.15f, 0.35f, 0.35f);
}

std::atomic<bool> dllUpdating(false);
std::atomic<bool> injectorUpdating(false);
std::atomic<bool> updateFinished(false);
std::string updateStatus = "";

static void UpdateDllThread()
{
    dllUpdating = true;
    updateStatus = "Updating DLL...";
    if (BaseUtils::UpdateDll(g_oldDllPath)) updateStatus = "DLL Updated!";
    else updateStatus = "DLL Update Failed!";
    dllUpdating = false;
}

static void UpdateInjectorThread()
{
    injectorUpdating = true;
    updateStatus = "Updating Loader...";
    if (BaseUtils::UpdateInjector()) {
        updateStatus = "Loader Updated!";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    } else {
        updateStatus = "Loader Update Failed!";
    }
    injectorUpdating = false;
}

static void AnimateUpdateStatus()
{
    static int dotCount = 3;
    static int lastUpdateTime = 0;
    int currentTime = static_cast<int>(std::chrono::system_clock::now().time_since_epoch().count() / 1000000000);
    if (currentTime - lastUpdateTime >= 1) { dotCount = (dotCount + 1) % 4; lastUpdateTime = currentTime; }
    std::string dotStr(dotCount, '.');
    if (dllUpdating && injectorUpdating) updateStatus = "Updating DLL and Loader" + dotStr;
    else if (dllUpdating) updateStatus = "Updating DLL" + dotStr;
    else if (injectorUpdating) updateStatus = "Updating Loader" + dotStr;
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
            std::string errMsg = "Injection failed.\n\n" + g_injectError;
            Logger::GetInstance().Write("[loader] injection failed: " + g_injectError);
            MessageBoxA(nullptr, errMsg.c_str(), "cutie - Injection Failed", MB_OK | MB_ICONERROR);
        }
    }

    if (g_injecting && !g_injectFinished)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_injectStartTime).count();
        if (elapsed > 45)
        {
            g_injecting = false;
            Logger::GetInstance().Write("[loader] injection timed out");
            MessageBoxA(nullptr, "Injection timed out.\n\nMake sure the loader folder is excluded in Defender.",
                "cutie - Timeout", MB_OK | MB_ICONERROR);
        }
    }

    static bool update = BaseUtils::IsInjectorUpdated();
    static bool dllUpdated = false;

    static std::vector<ProcessManager::WindowInfo> processes;
    static int selectedProcess = 0;

    static int frameCount = 0;
    frameCount++;
    if (frameCount == 1 || frameCount % 30 == 0)
    {
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
    float baseH = fmodf(t * 0.14f, 1.f);

    if (!g_sparksInit)
    {
        srand(1337);
        for (auto& s : g_sparks)
        {
            s.x = (float)(rand() % (int)(winW > 0 ? winW : 700));
            s.y = (float)(rand() % (int)(winH > 0 ? winH : 400));
            s.vy = -(10.f + rand() % 35);
            s.life = (float)(rand() % 100) / 100.f * 3.f;
            s.maxLife = 2.5f + (float)(rand() % 200) / 100.f;
            s.sz = 0.8f + (float)(rand() % 20) / 10.f;
            s.hue = 0.72f + (float)(rand() % 28) / 100.f;
        }
        g_sparksInit = true;
    }

    for (auto& s : g_sparks)
    {
        s.life += dt; s.y += s.vy * dt;
        if (s.life >= s.maxLife || s.y < -10.f)
        {
            s.x = (float)(rand() % (int)(winW > 0 ? winW : 700));
            s.y = winH + 5.f;
            s.vy = -(10.f + rand() % 45);
            s.life = 0.f;
            s.maxLife = 2.f + (float)(rand() % 300) / 100.f;
            s.sz = 0.8f + (float)(rand() % 20) / 10.f;
            s.hue = 0.72f + (float)(rand() % 28) / 100.f;
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
    dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + winW, wp.y + winH), IM_COL32(7, 5, 14, 255));

    float gp = 0.5f + 0.5f * sinf(t * 0.7f);
    dl->AddCircleFilled(ImVec2(wp.x + winW * 0.5f, wp.y + winH * 0.4f), winH * 0.55f, IM_COL32(55, 20, 100, (int)(18 + 12 * gp)), 64);

    for (auto& s : g_sparks)
    {
        float alpha = sinf(s.life / s.maxLife * 3.14159f);
        ImU32 sc = HSVU32(s.hue + sinf(t * 0.25f) * 0.05f, 0.75f, 1.f, alpha * 0.65f);
        dl->AddCircleFilled(ImVec2(wp.x + s.x, wp.y + s.y), s.sz, sc, 5);
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
    float titleSz = fntSz * 2.6f;
    float pulse = 0.5f + 0.5f * sinf(t * 2.2f);
    float cursor = 18.f;

    const char* title = "cutie";
    float titleTotalW = fnt->CalcTextSizeA(titleSz, FLT_MAX, 0.f, title, nullptr, nullptr).x;
    float tx = cp.x + (cW - titleTotalW) * 0.5f;
    float ty = cp.y + cursor;
    for (int g = 5; g >= 1; g--)
    {
        float spread = (float)g * 2.f;
        float ga = (0.06f + 0.03f * pulse) / (float)g;
        ImU32 gc = HSVU32(baseH, 0.8f, 1.f, ga);
        for (float ddx = -spread; ddx <= spread; ddx += spread)
            for (float ddy = -spread; ddy <= spread; ddy += spread)
                cdl->AddText(fnt, titleSz, ImVec2(tx + ddx, ty + ddy), gc, title);
    }
    float cx2 = tx;
    for (int i = 0; title[i]; i++)
    {
        char ch[2] = { title[i], 0 };
        float cw2 = fnt->CalcTextSizeA(titleSz, FLT_MAX, 0.f, ch, nullptr, nullptr).x;
        cdl->AddText(fnt, titleSz, ImVec2(cx2, ty), HSVU32(fmodf(baseH + (float)i * 0.09f, 1.f), 0.65f + 0.25f * sinf(t * 1.8f + (float)i * 0.9f), 0.85f + 0.15f * sinf(t * 2.5f + (float)i * 1.1f), 1.f), ch);
        cx2 += cw2;
    }
    cursor += fnt->CalcTextSizeA(titleSz, FLT_MAX, 0.f, title, nullptr, nullptr).y + 6.f;

    const char* sub = "the cutest injector";
    float subSz = fntSz * 0.78f;
    float subW = fnt->CalcTextSizeA(subSz, FLT_MAX, 0.f, sub, nullptr, nullptr).x;
    cdl->AddText(fnt, subSz, ImVec2(cp.x + (cW - subW) * 0.5f, cp.y + cursor), HSVU32(baseH, 0.4f, 0.8f, 0.32f + 0.14f * sinf(t * 1.1f)), sub);
    cursor += fnt->CalcTextSizeA(subSz, FLT_MAX, 0.f, sub, nullptr, nullptr).y + 20.f;

    {
        float sepPulse = 0.5f + 0.5f * sinf(t * 1.6f);
        float sx0 = cp.x + 15.f, sx1 = cp.x + cW - 15.f, sy = cp.y + cursor;
        int segs = 50;
        for (int i = 0; i < segs; i++)
        {
            float f0 = (float)i / segs, f1 = (float)(i + 1) / segs;
            float x0 = sx0 + f0 * (sx1 - sx0), x1 = sx0 + f1 * (sx1 - sx0);
            float dist = fabsf((f0 + f1) * 0.5f - 0.5f) * 2.f;
            cdl->AddLine(ImVec2(x0, sy), ImVec2(x1, sy), HSVU32(fmodf(baseH + f0 * 0.12f, 1.f), 0.8f, 1.f, (1.f - dist) * (0.45f + 0.45f * sepPulse)), 1.5f);
        }
        cursor += 18.f;
    }

    {
        float bdH = 24.f, gap = 8.f;
        float bdW = (cW - gap - 30.f) * 0.5f;
        float bd1X = cp.x + 15.f, bd2X = bd1X + bdW + gap, bdY = cp.y + cursor;
        float bdPulse = 0.5f + 0.5f * sinf(t * 1.9f + 0.3f);

        cdl->AddRectFilled(ImVec2(bd1X, bdY), ImVec2(bd1X + bdW, bdY + bdH), IM_COL32(30, 15, 60, 210), 12.f);
        cdl->AddRect(ImVec2(bd1X, bdY), ImVec2(bd1X + bdW, bdY + bdH), HSVU32(baseH, 0.75f, 0.9f, 0.4f + 0.3f * bdPulse), 12.f, 0, 1.f);
        cdl->AddRectFilled(ImVec2(bd2X, bdY), ImVec2(bd2X + bdW, bdY + bdH), IM_COL32(15, 20, 55, 210), 12.f);
        cdl->AddRect(ImVec2(bd2X, bdY), ImVec2(bd2X + bdW, bdY + bdH), HSVU32(fmodf(baseH + 0.07f, 1.f), 0.65f, 0.85f, 0.35f + 0.25f * bdPulse), 12.f, 0, 1.f);

        float lblSz = fntSz * 0.82f;
        float lblH = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, "X", nullptr, nullptr).y;
        std::string dllLbl = "dll  " + dllVersion;
        std::string ldrLbl = "loader  " + injectorVersion;
        float dlW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, dllLbl.c_str(), nullptr, nullptr).x;
        float ldW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, ldrLbl.c_str(), nullptr, nullptr).x;
        cdl->AddText(fnt, lblSz, ImVec2(bd1X + (bdW - dlW) * 0.5f, bdY + (bdH - lblH) * 0.5f), HSVU32(baseH, 0.3f, 0.95f, 1.f), dllLbl.c_str());
        cdl->AddText(fnt, lblSz, ImVec2(bd2X + (bdW - ldW) * 0.5f, bdY + (bdH - lblH) * 0.5f), HSVU32(fmodf(baseH + 0.07f, 1.f), 0.3f, 0.95f, 1.f), ldrLbl.c_str());
        cursor += bdH + 20.f;
    }

    {
        std::string tgt = processes.size() > 0 ? processes[selectedProcess].processName : "watching for Minecraft...";
        float tgtAlpha = minecraft_running ? 1.f : 0.4f;
        float lblSz = fntSz * 0.88f;
        const char* lbl = "target  ";
        float lblW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, lbl, nullptr, nullptr).x;
        float tgtW = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, tgt.c_str(), nullptr, nullptr).x;
        float lblH = fnt->CalcTextSizeA(lblSz, FLT_MAX, 0.f, "X", nullptr, nullptr).y;
        float startX = cp.x + (cW - lblW - tgtW) * 0.5f;
        cdl->AddText(fnt, lblSz, ImVec2(startX, cp.y + cursor), HSVU32(baseH, 0.5f, 0.75f, tgtAlpha * 0.65f), lbl);
        cdl->AddText(fnt, lblSz, ImVec2(startX + lblW, cp.y + cursor), HSVU32(0.f, 0.f, 1.f, tgtAlpha * 0.9f), tgt.c_str());
        cursor += lblH + 20.f;
    }

    {
        float btnH = 44.f;
        float injPulse = 0.5f + 0.5f * sinf(t * 2.8f);
        float btn1X = cp.x + 15.f, btn1W = cW - 30.f;

        for (int g = 5; g >= 1; g--)
        {
            float exp2 = (float)g * 2.5f;
            float ga = (0.07f + 0.05f * injPulse) / (float)g;
            ImU32 gc = HSVU32(fmodf(baseH + 0.05f, 1.f), 0.85f, 1.f, ga);
            cdl->AddRectFilled(ImVec2(btn1X - exp2, cp.y + cursor - exp2), ImVec2(btn1X + btn1W + exp2, cp.y + cursor + btnH + exp2), gc, 10.f + exp2);
        }

        if (g_injecting)
        {
            const char* injText = "injecting...";
            float injW = fnt->CalcTextSizeA(fntSz * 1.1f, FLT_MAX, 0.f, injText, nullptr, nullptr).x;
            cdl->AddText(fnt, fntSz * 1.1f, ImVec2(cp.x + (cW - injW) * 0.5f, cp.y + cursor + (btnH - fntSz * 1.1f) * 0.5f), HSVU32(0.15f, 0.9f, 0.95f, 1.f), injText);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, HSV(fmodf(baseH + 0.05f, 1.f), 0.72f, 0.30f + 0.10f * injPulse, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HSV(fmodf(baseH + 0.05f, 1.f), 0.72f, 0.50f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, HSV(fmodf(baseH + 0.05f, 1.f), 0.80f, 0.65f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
            ImGui::SetCursorPos(ImVec2(15.f, cursor));
            if (ImGui::Button(minecraft_running ? "inject" : "watching...", ImVec2(btn1W, btnH)))
            {
                if (!minecraft_running)
                {
                }
                else
                {
                    std::string dllPath = ProcessManager::GetDllPath();
                    if (dllPath.empty()) dllPath = FolderManager::GetDllPath();

                    if (dllPath.empty())
                    {
                        Logger::GetInstance().Write("[loader] DLL not found");
                        MessageBoxA(nullptr, "cutie.dll not found.\nRe-extract the loader from its archive.", "cutie - DLL Missing", MB_OK | MB_ICONERROR);
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
        cursor += btnH + 10.f;
    }

    if (multiple_minecraft_instances)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.08f, 0.20f, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.16f, 0.38f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.32f, 0.22f, 0.52f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 6.f));
        ImGui::SetCursorPos(ImVec2(15.f, cursor));
        if (ImGui::Button("switch instance", ImVec2(cW - 30.f, 0.f)))
        {
            selectedProcess++;
            if (selectedProcess >= (int)processes.size()) selectedProcess = 0;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        cursor += ImGui::GetFrameHeight() + 8.f;
    }

    AnimateUpdateStatus();
    if (dllUpdating || injectorUpdating)
    {
        float stSz = fntSz * 0.85f;
        float stW = fnt->CalcTextSizeA(stSz, FLT_MAX, 0.f, updateStatus.c_str(), nullptr, nullptr).x;
        cdl->AddText(fnt, stSz, ImVec2(cp.x + (cW - stW) * 0.5f, cp.y + cursor), HSVU32(fmodf(baseH + 0.12f, 1.f), 0.55f, 0.9f, 0.85f), updateStatus.c_str());
    }

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