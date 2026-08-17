#pragma once

#include "moduleManager/moduleBase.h"
#include <jni/jni.h>
#include <map>
#include <string>
#include <queue>
#include <deque>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <windows.h>
#include <winhttp.h>

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

    bool isValid = false;
    bool hasValidStats = false;
    bool leaderboardFetched = false;
    bool profileFetched = false;
    bool profileExists = false;
    std::chrono::system_clock::time_point fetchTime;
};

// used for splitting endpoints across the proxy pool
enum class FetchTaskType {
    LEADERBOARD,
    PROFILE
};

struct FetchTask {
    std::string name;
    FetchTaskType type;
    std::chrono::steady_clock::time_point executeAfter;
};

class TabList : public ModuleBase {
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

    static std::queue<std::string> fetchQueue;
    static std::mutex queueMutex;
    static std::string BuildStatSuffix(const TabListStatsData& stats);

    static void EnqueuePlayer(const std::string& name);
    static void ClearTaskQueues();

private:
    static std::atomic<bool> threadRunning;
    static std::vector<std::thread> workerThreads; // threading pool

    static std::deque<FetchTask> highPriorityQueue;
    static std::deque<FetchTask> lowPriorityQueue;
    static std::mutex taskQueueMutex;
    static std::condition_variable taskCv;

    static HINTERNET hHttpSession;
    static HINTERNET hHttpConnect;
    static std::mutex httpInitMutex;

    static void InitHttpPool();
    static void CloseHttpPool();
    static std::string WinHttpGetPath(const std::string& path, int& statusCode);

    static void PushTask(const std::string& name, FetchTaskType type, int delayMs = 0);
    static bool PopTask(FetchTask& task);

    static void WorkerThreadFunc();
    static void FetchPlayerStats(const std::string& playerName);
    static void FetchLeaderboardStats(const std::string& playerName);
    static void FetchProfileStats(const std::string& playerName);
    static void ExecuteLeaderboardTask(const std::string& playerName);
    static void ExecuteProfileTask(const std::string& playerName);

    static void InjectAllTabStats();
    static void InjectTabWatermarkFooter();
    static void ClearTabWatermarkFooter();
    static std::string WinHttpGet(const std::string& url, int& statusCode);
    static void ClearAllDisplayNames();
    static bool ShouldShowStats(jobject mc);
};