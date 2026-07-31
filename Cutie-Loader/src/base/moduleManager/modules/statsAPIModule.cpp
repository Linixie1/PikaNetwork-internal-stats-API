#include "statsAPIModule.h"
#include "moduleManager/moduleManager.h"
#include "java/java.h"
#include "sdk/strayCache.h"
#include "menu/menu.h"
#include "json/json.hpp"
#include "../../../denick/denickModule.h"
#include "../../../denick/methodOne.h"
#include "../../../denick/methodTwo.h"
#include "../../../denick/methodThree.h"
#include "moduleManager/commonData.h"
#include <Windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <fstream>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

char StatsAPIModule::playerSearchBuffer[64] = "";
char StatsAPIModule::clanSearchBuffer[64] = "";
int StatsAPIModule::selectedInterval = 0;
int StatsAPIModule::selectedMode = 3;

std::string StatsAPIModule::playerSearchResult = "Awaiting search...";
std::string StatsAPIModule::clanSearchResult = "Awaiting search...";

std::map<std::string, CachedStats> StatsAPIModule::statsCache;
std::mutex StatsAPIModule::cacheMutex;
std::mutex StatsAPIModule::guiMutex;

// All stat display toggles
bool StatsAPIModule::showLevel = true;
bool StatsAPIModule::showGuildTab = true;
bool StatsAPIModule::showFkdr = true;
bool StatsAPIModule::showKdr = true;
bool StatsAPIModule::showFinalKills = false;
bool StatsAPIModule::showFinalDeaths = false;
bool StatsAPIModule::showKills = false;
bool StatsAPIModule::showDeaths = false;
bool StatsAPIModule::showWins = false;
bool StatsAPIModule::showLosses = false;
bool StatsAPIModule::showWlr = false;
bool StatsAPIModule::showBedsBroken = false;
bool StatsAPIModule::showWinstreak = false;
bool StatsAPIModule::showGamesPlayed = false;
bool StatsAPIModule::showArrowsShot = false;
bool StatsAPIModule::showArrowsHit = false;
bool StatsAPIModule::showMissedShots = false;
bool StatsAPIModule::showBowKills = false;
bool StatsAPIModule::showVoidKills = false;
bool StatsAPIModule::showMeleeKills = false;

CachedStats StatsAPIModule::lastDisplayedStats;
ClanSearchData StatsAPIModule::clanSearchData;

static const char* intervalNames[] = { "Lifetime", "Monthly", "Weekly", "Yearly" };
static const char* intervalApiKeys[] = { "lifetime", "monthly", "weekly", "yearly" };
static const char* modeNames[] = { "Solos", "Duos", "Quads", "All Modes" };
static const char* modeApiKeys[] = { "SOLO", "DOUBLES", "QUADS", "ALL_MODES" };

// Keep all Unicode including emojis like ✽ (mainly for guild tags)
static std::string CleanTag(const std::string& in) {
    return in;
}

static std::string GetAutoSavePath() {
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path) == S_OK) {
        return std::string(path) + "\\.cutie\\configs\\cutie-stats.cfg";
    }
    return "C:\\.cutie\\configs\\cutie-stats.cfg";
}

void StatsAPIModule::AutoLoad() {
    std::string path = GetAutoSavePath();
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        json j = json::parse(f);
        StatsAPIModule::selectedInterval = j.value("interval", 0);
        StatsAPIModule::selectedMode = j.value("mode", 3);
        showLevel = j.value("showLevel", true);
        showGuildTab = j.value("showGuildTab", true);
        showFkdr = j.value("showFkdr", true);
        showKdr = j.value("showKdr", true);
        showFinalKills = j.value("showFinalKills", false);
        showFinalDeaths = j.value("showFinalDeaths", false);
        showKills = j.value("showKills", false);
        showDeaths = j.value("showDeaths", false);
        showWins = j.value("showWins", false);
        showLosses = j.value("showLosses", false);
        showWlr = j.value("showWlr", false);
        showBedsBroken = j.value("showBedsBroken", true);
        showWinstreak = j.value("showWinstreak", false);
        showGamesPlayed = j.value("showGamesPlayed", false);
        showArrowsShot = j.value("showArrowsShot", false);
        showArrowsHit = j.value("showArrowsHit", false);
        showMissedShots = j.value("showMissedShots", false);
        showBowKills = j.value("showBowKills", false);
        showVoidKills = j.value("showVoidKills", false);
        showMeleeKills = j.value("showMeleeKills", false);
    } catch (...) {}
    f.close();
}

void StatsAPIModule::AutoSave() {
    json j;
    j["interval"] = StatsAPIModule::selectedInterval;
    j["mode"] = StatsAPIModule::selectedMode;
    j["showLevel"] = showLevel;
    j["showGuildTab"] = showGuildTab;
    j["showFkdr"] = showFkdr;
    j["showKdr"] = showKdr;
    j["showFinalKills"] = showFinalKills;
    j["showFinalDeaths"] = showFinalDeaths;
    j["showKills"] = showKills;
    j["showDeaths"] = showDeaths;
    j["showWins"] = showWins;
    j["showLosses"] = showLosses;
    j["showWlr"] = showWlr;
    j["showBedsBroken"] = showBedsBroken;
    j["showWinstreak"] = showWinstreak;
    j["showGamesPlayed"] = showGamesPlayed;
    j["showArrowsShot"] = showArrowsShot;
    j["showArrowsHit"] = showArrowsHit;
    j["showMissedShots"] = showMissedShots;
    j["showBowKills"] = showBowKills;
    j["showVoidKills"] = showVoidKills;
    j["showMeleeKills"] = showMeleeKills;
    std::string path = GetAutoSavePath();
    std::ofstream f(path);
    f << j.dump(4);
    f.close();
}

StatsAPIModule::StatsAPIModule() { AutoLoad(); }
StatsAPIModule::~StatsAPIModule() {
    AutoSave();
    while (StatsAPIModule::activeApiThreads > 0) {
        Sleep(50);
    }
}

static std::string MakeCacheKey(const std::string& username, int interval, int mode) {
    return username + "_i" + std::to_string(interval) + "_m" + std::to_string(mode);
}

CachedStats* StatsAPIModule::GetCachedStats(const std::string& username, int interval, int mode) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = statsCache.find(MakeCacheKey(username, interval, mode));
    if (it != statsCache.end()) {
        auto age = std::chrono::steady_clock::now() - it->second.fetchTime;
        if (age < std::chrono::seconds(60)) return &it->second;
    }
    return nullptr;
}

void StatsAPIModule::SetCachedStats(const std::string& username, int interval, int mode, const CachedStats& stats) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    statsCache[MakeCacheKey(username, interval, mode)] = stats;
    statsCache[MakeCacheKey(username, interval, mode)].fetchTime = std::chrono::steady_clock::now();
}

static int ParseStatFromLB(const json& lb, const std::string& statName) {
    if (!lb.contains(statName)) return 0;
    auto& obj = lb[statName];
    if (!obj.contains("entries") || !obj["entries"].is_array() || obj["entries"].empty()) return 0;
    auto& entry = obj["entries"][0];
    if (!entry.contains("value")) return 0;
    std::string v = entry["value"].get<std::string>();
    v.erase(std::remove(v.begin(), v.end(), ','), v.end());
    try { return std::stoi(v); } catch (...) { return 0; }
}

static ImVec4 StatColor(float val, float low, float mid, float high) {
    if (val >= high)     return ImVec4(1.0f, 0.15f, 0.15f, 1.0f);
    if (val >= mid)      return ImVec4(1.0f, 0.6f, 0.05f, 1.0f);
    if (val >= low)      return ImVec4(1.0f, 0.85f, 0.1f, 1.0f);
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}
// Renders the stat cards inside the GUI
static void RenderCard(const char* label, const char* valueStr, const char* subStr, ImVec4 col, ImVec2 cardMin, float cardW, float cardH) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bgCol = ImGui::GetColorU32(ImGuiCol_ChildBg);
    ImVec2 cMax = ImVec2(cardMin.x + cardW, cardMin.y + cardH);
    dl->AddRectFilled(cardMin, cMax, bgCol, 6.f);
    dl->AddRect(cardMin, cMax, ImGui::GetColorU32(ImGuiCol_Border), 6.f);

    ImVec2 valSz = Menu::fontBold->CalcTextSizeA(20, FLT_MAX, 0.0f, valueStr);
    ImVec2 lblSz = Menu::font14->CalcTextSizeA(14, FLT_MAX, 0.0f, label);

    float cx = cardMin.x + (cardW - valSz.x) / 2.f;
    dl->AddText(Menu::fontBold, 20, ImVec2(cx, cardMin.y + 8.f), ImGui::GetColorU32(col), valueStr);
    cx = cardMin.x + (cardW - lblSz.x) / 2.f;
    dl->AddText(Menu::font14, 14, ImVec2(cx, cardMin.y + 8.f + valSz.y + 1.f), ImGui::GetColorU32(ImVec4(0.55f, 0.55f, 0.55f, 1.0f)), label);
    if (subStr && subStr[0]) {
        ImVec2 subSz = Menu::font14->CalcTextSizeA(14, FLT_MAX, 0.0f, subStr);
        cx = cardMin.x + (cardW - subSz.x) / 2.f;
        dl->AddText(Menu::font14, 14, ImVec2(cx, cardMin.y + 8.f + valSz.y + 1.f + lblSz.y + 2.f), IM_COL32(120, 120, 120, 255), subStr);
    }
}
// Lets you search for the username (and allows pressing enter, without needing to click with your mouse on the button)
static bool DoSearchAction(const char* searchBuf, std::string& resultOut, int interval, int mode,
    void (*fetchFunc)(std::string, int, int)) {
    if (searchBuf[0] == 0) return false;
    std::string t(searchBuf);
    resultOut = "Fetching " + t + " [" +
        std::string(intervalNames[interval]) + "/" +
        std::string(modeNames[mode]) + "]...";
    StatsAPIModule::activeApiThreads++;
    std::thread([fetchFunc, t, interval, mode]() {
        fetchFunc(t, interval, mode);
        StatsAPIModule::activeApiThreads--;
    }).detach();
    return true;
}

void StatsAPIModule::RenderMenu() {

    // Lets you choose what cards to actually display (the good deafult is inside the loaders resource)
    Menu::HorizontalSeparatorText("Stat Cards to Display", FontSize::SIZE_16);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 4.f));
        ImGui::Columns(2, nullptr, false);
        
        if (Menu::Checkbox("FKDR", &showFkdr)) AutoSave();
        if (Menu::Checkbox("KDR", &showKdr)) AutoSave();
        if (Menu::Checkbox("Final Kills", &showFinalKills)) AutoSave();
        if (Menu::Checkbox("Final Deaths", &showFinalDeaths)) AutoSave();
        if (Menu::Checkbox("Kills", &showKills)) AutoSave();
        if (Menu::Checkbox("Deaths", &showDeaths)) AutoSave();
        if (Menu::Checkbox("Wins", &showWins)) AutoSave();
        if (Menu::Checkbox("Losses", &showLosses)) AutoSave();
        if (Menu::Checkbox("WLR", &showWlr)) AutoSave();

        ImGui::NextColumn();
        
        if (Menu::Checkbox("Beds Broken", &showBedsBroken)) AutoSave();
        if (Menu::Checkbox("Winstreak", &showWinstreak)) AutoSave();
        if (Menu::Checkbox("Games", &showGamesPlayed)) AutoSave();
        if (Menu::Checkbox("Arrows Shot", &showArrowsShot)) AutoSave();
        if (Menu::Checkbox("Arrows Hit", &showArrowsHit)) AutoSave();
        if (Menu::Checkbox("Missed Shots", &showMissedShots)) AutoSave();
        if (Menu::Checkbox("Bow Kills", &showBowKills)) AutoSave();
        if (Menu::Checkbox("Void Kills", &showVoidKills)) AutoSave();
        if (Menu::Checkbox("Melee Kills", &showMeleeKills)) AutoSave();

        ImGui::Columns(1);
        ImGui::PopStyleVar();
    }

    float availW = ImGui::GetWindowSize().x - 40.f;
    if (Menu::Button("Clear Cache", ImVec2(availW, 26.f))) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        statsCache.clear();
    }
    ImGui::Spacing();

    // Button to search for stats
    Menu::HorizontalSeparatorText("Player BedWars Stats", FontSize::SIZE_18);

    float comboW = (availW - 12.f) / 2.f;
    ImGui::PushItemWidth(comboW);
    {
        if (ImGui::Combo("##IntervalCombo", &selectedInterval, intervalNames, 4)) AutoSave();
        ImGui::SameLine();
        if (ImGui::Combo("##ModeCombo", &selectedMode, modeNames, 4)) AutoSave();
    }
    ImGui::PopItemWidth();

    float btnSizeH = Menu::font18->CalcTextSizeA(18, FLT_MAX, 0.0f, "Search").x + ImGui::GetStyle().FramePadding.x * 4;
    float inputW = availW - btnSizeH - 8.f;
    ImGui::PushItemWidth(inputW);
    bool playerEnter = ImGui::InputText("##PlayerSearch", playerSearchBuffer, sizeof(playerSearchBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (Menu::Button("Search", ImVec2(btnSizeH, 0.f)) || playerEnter) {
        std::string searchStr(playerSearchBuffer);
        bool isValid = true;
        if (searchStr.length() < 3 || searchStr.length() > 16) isValid = false;
        else {
            for (char c : searchStr) {
                if (!isalnum((unsigned char)c) && c != '_') { isValid = false; break; }
            }
        }
        if (isValid) {
            DoSearchAction(playerSearchBuffer, playerSearchResult, selectedInterval, selectedMode, FetchPlayerStatsAPI);
        } else {
            std::lock_guard<std::mutex> lock(guiMutex);
            playerSearchResult = "[WARNING: Invalid username format (3-16 chars, letters/numbers/underscores only)]";
            lastDisplayedStats = CachedStats{}; 
        }
    }

    // Player stats card grid
    {
        std::lock_guard<std::mutex> lock(guiMutex);
        const std::string& result = playerSearchResult;

        bool isAwaiting = (result.find("Awaiting search") != std::string::npos);
        bool isFetching = (result.find("Fetching") != std::string::npos);
        bool isError = (result.find("not found") != std::string::npos ||
                        result.find("error") != std::string::npos ||
                        result.find("Rate limited") != std::string::npos ||
                        result.find("Failed") != std::string::npos);

        if (isAwaiting) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
            Menu::TextColored("Enter a player name and press Enter or click Search.", ImVec4(0.5f, 0.5f, 0.5f, 1.0f), FontSize::SIZE_16);
        } else if (isFetching) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
            Menu::TextColored("Fetching stats...", ImVec4(1.0f, 0.8f, 0.0f, 1.0f), FontSize::SIZE_16);
        } else if (isError) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
            Menu::TextColored(result.c_str(), ImVec4(1.0f, 0.3f, 0.3f, 1.0f), FontSize::SIZE_16);
        } else {
            CachedStats& s = lastDisplayedStats;

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
            {
                std::string header = s.username;
                if (!s.guildTag.empty() && s.guildTag != "None")
                    header += "  [" + s.guildTag + "]";
                if (s.level > 0)
                    header += "  |  Lvl " + std::to_string(s.level);
                Menu::BoldText(header.c_str(), FontSize::SIZE_20);
            }
            ImGui::Spacing();

            struct StatCard { const char* label; char value[20]; char sub[48]; ImVec4 color; };
            std::vector<StatCard> cards;

            auto addF = [&](const char* l, float v, const char* fmt, const char* sub, ImVec4 c) {
                StatCard sc; sc.label = l; sc.color = c;
                snprintf(sc.value, sizeof(sc.value), fmt, v);
                snprintf(sc.sub, sizeof(sc.sub), "%s", sub ? sub : "");
                cards.push_back(sc);
            };
            auto addI = [&](const char* l, int v, const char* sub, ImVec4 c) {
                StatCard sc; sc.label = l; sc.color = c;
                snprintf(sc.value, sizeof(sc.value), "%d", v);
                snprintf(sc.sub, sizeof(sc.sub), "%s", sub ? sub : "");
                cards.push_back(sc);
            };

            if (showFkdr)        addF("FKDR", s.fkdr, "%.2f", (std::to_string(s.finalKills) + " FK / " + std::to_string(s.finalDeaths) + " FD").c_str(), StatColor(s.fkdr, 1.f, 2.f, 5.f));
            if (showKdr)         addF("KDR", s.kdr, "%.2f", (std::to_string(s.kills) + " K / " + std::to_string(s.deaths) + " D").c_str(), StatColor(s.kdr, 1.f, 2.f, 5.f));
            if (showFinalKills)  addI("Final Kills", s.finalKills, nullptr, ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
            if (showFinalDeaths) addI("Final Deaths", s.finalDeaths, nullptr, ImVec4(0.4f, 0.4f, 0.9f, 1.0f));
            if (showKills)       addI("Kills", s.kills, nullptr, ImVec4(0.9f, 0.7f, 0.3f, 1.0f));
            if (showDeaths)      addI("Deaths", s.deaths, nullptr, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            if (showWins)        addI("Wins", s.wins, nullptr, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
            if (showLosses)      addI("Losses", s.losses, nullptr, ImVec4(0.9f, 0.5f, 0.5f, 1.0f));
            if (showBedsBroken)  addI("Beds Broken", s.bedsBroken, nullptr, ImVec4(0.3f, 0.85f, 0.95f, 1.0f));
            if (showWlr)         addF("WLR", s.wlr, "%.2f", (std::to_string(s.wins) + " W / " + std::to_string(s.losses) + " L").c_str(), StatColor(s.wlr, 0.5f, 1.0f, 3.0f));
            if (showWinstreak)   addI("Winstreak", s.winstreak, nullptr, StatColor((float)s.winstreak, 5.f, 20.f, 50.f));
            if (showGamesPlayed) addI("Games", s.gamesPlayed, nullptr, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            if (showArrowsShot)  addI("Arrows Shot", s.arrowsShot, nullptr, ImVec4(0.85f, 0.75f, 0.4f, 1.0f));
            if (showArrowsHit)   addI("Arrows Hit", s.arrowsHit, nullptr, ImVec4(0.85f, 0.85f, 0.4f, 1.0f));
            if (showMissedShots) addI("Missed Shots", s.missedShots, nullptr, ImVec4(0.85f, 0.5f, 0.3f, 1.0f));
            if (showBowKills)    addI("Bow Kills", s.bowKills, nullptr, ImVec4(0.8f, 0.9f, 0.2f, 1.0f));
            if (showVoidKills)   addI("Void Kills", s.voidKills, nullptr, ImVec4(0.5f, 0.3f, 0.8f, 1.0f));
            if (showMeleeKills)  addI("Melee Kills", s.meleeKills, nullptr, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));

            if (cards.empty()) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
                Menu::TextColored("No stat cards enabled. Use checkboxes above to show stats.", ImVec4(0.5f, 0.5f, 0.5f, 1.0f), FontSize::SIZE_16);
            } else {
                float cardPad = 6.f;
                float cardW = (availW - cardPad) / 2.f;
                float cardH = 62.f;
                for (size_t i = 0; i < cards.size(); ) {
                    ImVec2 rowBase = ImGui::GetCursorScreenPos();
                    RenderCard(cards[i].label, cards[i].value, cards[i].sub, cards[i].color, rowBase, cardW, cardH);
                    i++;
                    if (i < cards.size()) {
                        ImVec2 cMin2 = ImVec2(rowBase.x + cardW + cardPad, rowBase.y);
                        RenderCard(cards[i].label, cards[i].value, cards[i].sub, cards[i].color, cMin2, cardW, cardH);
                        i++;
                    }
                    ImGui::SetCursorScreenPos(ImVec2(rowBase.x, rowBase.y + cardH + cardPad));
                }
            }

            if (!s.clan.name.empty()) {
                ImGui::Spacing();
                ImGui::Spacing();
                Menu::HorizontalSeparatorText("Clan Details", FontSize::SIZE_16);
                
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
                Menu::BoldText((s.clan.name + "  [" + s.clan.tag + "]").c_str(), FontSize::SIZE_18);
                
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Owner:"); ImGui::SameLine(); ImGui::Text("%s", s.clan.owner.c_str()); ImGui::SameLine(200.f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Level:"); ImGui::SameLine(); ImGui::Text("%d", s.clan.level);
                
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Members:"); ImGui::SameLine(); ImGui::Text("%d", s.clan.members); ImGui::SameLine(200.f);
                
                std::string cDate = s.clan.creationTime;
                if (cDate.find('T') != std::string::npos) cDate = cDate.substr(0, cDate.find('T')); // Just get date part
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Created:"); ImGui::SameLine(); ImGui::Text("%s", cDate.c_str());
            }

            if (result.find("(cached)") != std::string::npos) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);
                Menu::TextColored("(cached)", ImVec4(0.5f, 0.5f, 0.5f, 1.0f), FontSize::SIZE_14);
            }
        }
    }

    // for clan stats (under player sats)
    ImGui::Spacing();
    Menu::HorizontalSeparatorText("Clan Stats", FontSize::SIZE_18);
    float cBtnSize = Menu::font18->CalcTextSizeA(18, FLT_MAX, 0.0f, "Search").x + ImGui::GetStyle().FramePadding.x * 4;
    float cInputW = availW - cBtnSize - 8.f;
    ImGui::PushItemWidth(cInputW);
    bool clanEnter = ImGui::InputText("##ClanSearch", clanSearchBuffer, sizeof(clanSearchBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (Menu::Button("Search ##Clan", ImVec2(cBtnSize, 0.f)) || clanEnter) {
        std::string t(clanSearchBuffer);
        if (!t.empty()) {
            clanSearchResult = "Fetching clan: " + t + "...";
            StatsAPIModule::activeApiThreads++;
            std::thread([t]() {
                FetchClanStatsAPI(t);
                StatsAPIModule::activeApiThreads--;
            }).detach();
        }
    }
    {
        std::lock_guard<std::mutex> lock(guiMutex);
        const std::string& res = clanSearchResult;
        if (!res.empty()) {
            if (res.find("Fetching") != std::string::npos) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
                Menu::TextColored(res.c_str(), ImVec4(1.0f, 0.8f, 0.0f, 1.0f), FontSize::SIZE_16);
            } else if (res.find("error") != std::string::npos || res.find("not found") != std::string::npos || res.find("Failed") != std::string::npos || res.find("limited") != std::string::npos) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
                Menu::TextColored(res.c_str(), ImVec4(1.0f, 0.3f, 0.3f, 1.0f), FontSize::SIZE_16);
            } else if (clanSearchData.hasData) {
                ImGui::Spacing();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
                Menu::BoldText((clanSearchData.name + "  [" + clanSearchData.tag + "]").c_str(), FontSize::SIZE_18);
                
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Owner:"); ImGui::SameLine(); ImGui::Text("%s", clanSearchData.owner.c_str()); ImGui::SameLine(200.f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Level:"); ImGui::SameLine(); ImGui::Text("%d", clanSearchData.level);
                
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Exp:"); ImGui::SameLine(); ImGui::Text("%d / %d", clanSearchData.exp, clanSearchData.totalExp); ImGui::SameLine(200.f);
                std::string cDate = clanSearchData.creationTime;
                if (cDate.find('T') != std::string::npos) cDate = cDate.substr(0, cDate.find('T'));
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Created:"); ImGui::SameLine(); ImGui::Text("%s", cDate.c_str());

                ImGui::Spacing();
                ImGui::Spacing();
                Menu::HorizontalSeparatorText(("Members (" + std::to_string(clanSearchData.membersCount) + ")").c_str(), FontSize::SIZE_16);
                ImGui::Spacing();
                
                if (ImGui::BeginChild("ClanMembersList", ImVec2(0, 150), true)) {
                    int columns = 3;
                    ImGui::Columns(columns, nullptr, false);
                    for (const auto& memberName : clanSearchData.memberNames) {
                        Menu::TextColored(memberName.c_str(), ImVec4(0.6f, 0.9f, 1.0f, 1.0f), FontSize::SIZE_16);
                        ImGui::NextColumn();
                    }
                    ImGui::Columns(1);
                }
                ImGui::EndChild();
            }
        }
    }
}

void StatsAPIModule::FetchPlayerStatsAPI(std::string username, int interval, int mode) {
    CachedStats* cached = GetCachedStats(username, interval, mode);
    if (cached) {
        std::lock_guard<std::mutex> lock(guiMutex);
        lastDisplayedStats = *cached;
        char buf[640];
        snprintf(buf, sizeof(buf),
            "Name: %s\nGuild: %s | Level: %d\n"
            "FKDR: %.2f (%d FK / %d FD) | KDR: %.2f (%d K / %d D)\n"
            "Wins: %d | Losses: %d | Beds: %d | WS: %d | Games: %d",
            cached->username.c_str(), cached->guildTag.c_str(), cached->level,
            cached->fkdr, cached->finalKills, cached->finalDeaths,
            cached->kdr, cached->kills, cached->deaths,
            cached->wins, cached->losses, cached->bedsBroken, cached->winstreak,
            cached->gamesPlayed);
        playerSearchResult = std::string(buf) + "\n(cached)";
        return;
    }

    CachedStats stats;
    stats.fetchTime = std::chrono::steady_clock::now();
    int code = 0, lbCode = 0;
    std::string resultText, lbJson, lbUrl;

    std::string profileUrl = "https://stats.pika-network.net/api/profile/" + username;
    std::string profileJson = WinHttpGet(profileUrl, code);
    if (code == 404) { resultText = "Player not found or nicked."; goto done; }
    if (code == 429) { resultText = "Rate limited. Wait."; goto done; }
    if (code != 200 || profileJson.empty()) { resultText = "Profile API error: " + std::to_string(code); goto done; }

    try {
        json p = json::parse(profileJson);
        stats.username = p.value("username", username);
        if (p.contains("rank") && p["rank"].contains("level"))
            stats.level = p["rank"]["level"].get<int>();
            
        if (p.contains("clan") && !p["clan"].is_null()) {
            auto& clanObj = p["clan"];
            stats.guildTag = CleanTag(clanObj.value("tag", ""));
            stats.clan.name = clanObj.value("name", "");
            stats.clan.tag = stats.guildTag;
            stats.clan.creationTime = clanObj.value("creationTime", "Unknown");
            
            if (clanObj.contains("owner") && clanObj["owner"].contains("username"))
                stats.clan.owner = clanObj["owner"]["username"].get<std::string>();
                
            if (clanObj.contains("leveling") && clanObj["leveling"].contains("level"))
                stats.clan.level = clanObj["leveling"]["level"].get<int>();
                
            if (clanObj.contains("members") && clanObj["members"].is_array())
                stats.clan.members = (int)clanObj["members"].size();
        } else {
            stats.guildTag = "None";
        }
    } catch (...) { stats.username = username; stats.guildTag = "None"; }

    lbUrl = "https://stats.pika-network.net/api/profile/" + stats.username +
        "/leaderboard?type=bedwars&interval=" + intervalApiKeys[interval] +
        "&mode=" + modeApiKeys[mode];
    lbJson = WinHttpGet(lbUrl, lbCode);

    if (lbCode == 200 && !lbJson.empty()) {
        try {
            json lb = json::parse(lbJson);
            stats.finalKills = ParseStatFromLB(lb, "Final kills");
            stats.finalDeaths = ParseStatFromLB(lb, "Final deaths");
            stats.kills = ParseStatFromLB(lb, "Kills");
            stats.deaths = ParseStatFromLB(lb, "Deaths");
            stats.wins = ParseStatFromLB(lb, "Wins");
            stats.losses = ParseStatFromLB(lb, "Losses");
            stats.bedsBroken = ParseStatFromLB(lb, "Beds destroyed");
            stats.gamesPlayed = ParseStatFromLB(lb, "Games played");
            stats.winstreak = ParseStatFromLB(lb, "Highest winstreak reached");
            stats.arrowsShot = ParseStatFromLB(lb, "Arrows shot");
            stats.arrowsHit = ParseStatFromLB(lb, "Arrows hit");
            stats.bowKills = ParseStatFromLB(lb, "Bow kills");
            stats.voidKills = ParseStatFromLB(lb, "Void kills");
            stats.meleeKills = ParseStatFromLB(lb, "Melee kills");
            stats.fkdr = stats.finalDeaths > 0 ? (float)stats.finalKills / stats.finalDeaths : (float)stats.finalKills;
            stats.kdr = stats.deaths > 0 ? (float)stats.kills / stats.deaths : (float)stats.kills;
            stats.wlr = stats.losses > 0 ? (float)stats.wins / stats.losses : (float)stats.wins;
            stats.missedShots = stats.arrowsShot - stats.arrowsHit;
            if (stats.missedShots < 0) stats.missedShots = 0;
        } catch (...) {}
    }

    SetCachedStats(stats.username, interval, mode, stats);
    lastDisplayedStats = stats;

    char buf[640];
    snprintf(buf, sizeof(buf),
        "Name: %s\nGuild: %s | Level: %d\n"
        "FKDR: %.2f (%d FK / %d FD) | KDR: %.2f (%d K / %d D)\n"
        "Wins: %d | Losses: %d | Beds: %d | WS: %d | Games: %d [%s/%s]",
        stats.username.c_str(), stats.guildTag.c_str(), stats.level,
        stats.fkdr, stats.finalKills, stats.finalDeaths,
        stats.kdr, stats.kills, stats.deaths,
        stats.wins, stats.losses, stats.bedsBroken, stats.winstreak,
        stats.gamesPlayed, intervalNames[interval], modeNames[mode]);
    resultText = buf;

done:
    { std::lock_guard<std::mutex> lock(guiMutex); playerSearchResult = resultText; }
}

void StatsAPIModule::FetchClanStatsAPI(std::string clanName) {
    int code = 0;
    std::string clanJson = WinHttpGet("https://stats.pika-network.net/api/clans/" + clanName, code);
    
    ClanSearchData csd;
    csd.hasData = false;
    
    std::string resultText;

    if (code == 404) resultText = "Clan not found.";
    else if (code == 429) resultText = "Rate limited.";
    else if (code != 200) resultText = "Clan API error: " + std::to_string(code);
    else {
        try {
            json c = json::parse(clanJson);
            csd.hasData = true;
            csd.name = c.value("name", clanName);
            csd.tag = CleanTag(c.value("tag", ""));
            csd.creationTime = c.value("creationTime", "Unknown");
            
            if (c.contains("owner") && c["owner"].contains("username"))
                csd.owner = c["owner"]["username"].get<std::string>();
                
            if (c.contains("leveling") && c["leveling"].contains("level")) {
                csd.level = c["leveling"]["level"].get<int>();
                csd.exp = c["leveling"].value("exp", 0);
                csd.totalExp = c["leveling"].value("totalExp", 0);
            }
                
            if (c.contains("members") && c["members"].is_array()) {
                csd.membersCount = (int)c["members"].size();
                for (auto& m : c["members"]) {
                    if (m.contains("user") && m["user"].contains("username"))
                        csd.memberNames.push_back(m["user"]["username"].get<std::string>());
                }
            }
        } catch (...) { resultText = "Failed to parse clan JSON."; csd.hasData = false; }
        if (csd.hasData) resultText = "Success";
    }
    { 
        std::lock_guard<std::mutex> lock(guiMutex); 
        clanSearchResult = resultText; 
        clanSearchData = csd;
    }
}

std::string StatsAPIModule::WinHttpGet(const std::string& urlStr, int& statusCode) {
    std::string responseStr;
    statusCode = 0;
    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {0}, path[2048] = {0};
    urlComp.lpszHostName = host; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path; urlComp.dwUrlPathLength = 2048;
    std::wstring wurl(urlStr.begin(), urlStr.end());
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.length(), 0, &urlComp)) return responseStr;
    HINTERNET hS = (HINTERNET)ModuleManager::g_hSession;
    if (!hS) return responseStr;
    HINTERNET hC = WinHttpConnect(hS, host, urlComp.nPort, 0);
    if (hC) {
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, urlComp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (hR) {
            if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hR, NULL)) {
                DWORD sz = sizeof(DWORD);
                WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hR, &avail) && avail > 0) {
                    char* buf = new char[avail + 1]();
                    DWORD down = 0;
                    if (WinHttpReadData(hR, buf, avail, &down)) { buf[down] = 0; responseStr += buf; }
                    delete[] buf;
                }
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    return responseStr;
}

void StatsAPIModule::Update() {
    if (!m_enabled || !Java::env) return;
    if (!CommonData::IsSafeToRun()) return;
    auto players = CommonData::GetPlayers();
    for (auto& p : players) {
        if (p.name.empty()) continue;
        if (!GetCachedStats(p.name, selectedInterval, selectedMode)) {
            static std::string last;
            static auto lastTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (p.name != last || now - lastTime > std::chrono::seconds(3)) {
                last = p.name; lastTime = now;
                StatsAPIModule::activeApiThreads++;
                std::thread([name = p.name]() {
                    FetchPlayerStatsAPI(name, StatsAPIModule::selectedInterval, StatsAPIModule::selectedMode);
                    StatsAPIModule::activeApiThreads--;
                }).detach();
            }
        }
    }
}