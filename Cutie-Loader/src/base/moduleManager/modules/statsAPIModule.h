#ifndef STATS_API_MODULE_H
#define STATS_API_MODULE_H

#include "moduleManager/moduleBase.h"
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

struct ClanInfo {
    std::string name;
    std::string tag;
    std::string owner;
    int level = 0;
    int members = 0;
    std::string creationTime;
};

struct CachedStats {
    std::string username;
    int level = 0;
    float fkdr = 0.f;
    float kdr = 0.f;
    float wlr = 0.f;
    int finalKills = 0;
    int finalDeaths = 0;
    int kills = 0;
    int deaths = 0;
    int wins = 0;
    int losses = 0;
    int bedsBroken = 0;
    int gamesPlayed = 0;
    int winstreak = 0;
    int arrowsShot = 0;
    int arrowsHit = 0;
    int missedShots = 0;
    int bowKills = 0;
    int voidKills = 0;
    int meleeKills = 0;
    std::string guildTag;
    ClanInfo clan;
    std::chrono::steady_clock::time_point fetchTime;
};

struct ClanSearchData {
    bool hasData = false;
    std::string name;
    std::string tag;
    std::string owner;
    int level = 0;
    int membersCount = 0;
    std::string creationTime;
    int exp = 0;
    int totalExp = 0;
    std::vector<std::string> memberNames;
};

class StatsAPIModule : public ModuleBase {
public:
    StatsAPIModule();
    ~StatsAPIModule();

    void Update() override;
    void RenderOverlay() override {}
    void RenderHud() override {}
    void RenderMenu() override;

    std::string GetName() override { return m_name; }
    std::string GetCategory() override { return m_category; }
    int GetKey() override { return 0; }
    bool IsEnabled() override { return m_enabled; }
    void SetEnabled(bool enabled) override { m_enabled = enabled; }
    void Toggle() override { m_enabled = !m_enabled; }

    static void AutoSave();
    static void AutoLoad();

private:
    std::string m_name = "StatsAPI";
    std::string m_category = "Stats";
    bool m_enabled = true;

public:
    // Display toggles - all stats (public so DenickModule can read them)
    static bool showLevel, showGuildTab;
    static bool showFkdr, showKdr, showWlr;
    static bool showFinalKills, showFinalDeaths;
    static bool showKills, showDeaths;
    static bool showWins, showLosses;
    static bool showBedsBroken, showWinstreak;
    static bool showGamesPlayed, showArrowsShot;
    static bool showArrowsHit, showMissedShots, showBowKills;
    static bool showVoidKills, showMeleeKills;

private:
    static char playerSearchBuffer[64];
    static char clanSearchBuffer[64];
    static int selectedInterval;  // 0=lifetime, 1=monthly, 2=weekly, 3=yearly
    static int selectedMode;      // 0=solo,1=duo,2=quads,3=ALL_MODES

    static std::string playerSearchResult;
    static std::string clanSearchResult;
    static ClanSearchData clanSearchData;

    static std::map<std::string, CachedStats> statsCache;
    static std::mutex cacheMutex;
    static std::mutex guiMutex;

public:
    static inline std::atomic<int> activeApiThreads = 0;

    // Last known good stats for card display (avoids parsing string)
    static CachedStats lastDisplayedStats;

    static void FetchPlayerStatsAPI(std::string username, int interval, int mode);
    static void FetchClanStatsAPI(std::string clanName);
    static std::string WinHttpGet(const std::string& urlStr, int& statusCode);
    
    static CachedStats* GetCachedStats(const std::string& username, int interval, int mode);
    static void SetCachedStats(const std::string& username, int interval, int mode, const CachedStats& stats);
};

#endif