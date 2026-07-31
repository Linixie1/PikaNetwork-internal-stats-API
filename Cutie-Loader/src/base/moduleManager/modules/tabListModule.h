#pragma once

#include "moduleManager/moduleBase.h"
#include <jni/jni.h>
#include <map>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

struct TabListStatsData {
    int level = 0;
    float fkdr = 0.0f;
    float kdr = 0.0f;
    float wlr = 0.0f;
    int finalKills = 0;
    int finalDeaths = 0;
    int kills = 0;
    int deaths = 0;
    int wins = 0;
    int losses = 0;
    int bedsBroken = 0;
    int winstreak = 0;
    int gamesPlayed = 0;
    int arrowsShot = 0;
    int arrowsHit = 0;
    int missedShots = 0;
    int bowKills = 0;
    int voidKills = 0;
    int meleeKills = 0;
    std::string clanTag = "";
    bool isNicked = false;
    bool isStatsOff = false;
    bool isValid = false;
    bool hasValidStats = false;
    bool fetchCompleted = false;
    std::chrono::system_clock::time_point fetchTime;
};

class TabList : public ModuleBase
{
public:
    TabList();
    ~TabList();

    void Update() override;
    void RenderOverlay() override {}
    void RenderHud() override;
    void RenderMenu() override;

    std::string GetName() override { return "TabList"; }
    std::string GetCategory() override { return "Visual"; }
    int GetKey() override { return 0; }

    bool IsEnabled() override;
    void SetEnabled(bool enabled) override;
    void Toggle() override;

    static std::map<std::string, TabListStatsData> nameCache;
    static std::mutex cacheMutex;
    static void ClearCache();

private:
    static std::queue<std::string> fetchQueue;
    static std::mutex queueMutex;
    static std::atomic<bool> threadRunning;
    static std::thread workerThread;

    static std::atomic<int> tickCounter;
    static void WorkerThreadFunc();
    static void FetchPlayerStats(const std::string& playerName);
    static void FetchLeaderboardStats(const std::string& playerName);
    static void FetchProfileStats(const std::string& playerName);
    static void InjectAllTabStats();
    static void InjectTabWatermarkFooter();
    static void ClearTabWatermarkFooter();
    static std::string WinHttpGet(const std::string& url, int& statusCode);
    static std::string BuildStatSuffix(const TabListStatsData& stats);
    static void ClearAllDisplayNames();
    static bool ShouldShowStats(jobject mc);
};