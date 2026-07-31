#include "tabListModule.h"
#include "../../../menu/menu.h"
#include "../../../configManager/settings.h"
#include "../../../util/logger.h"
#include "../../../sdk/strayCache.h"
#include "../../../sdk/jni_safety.h"
#include "../../../java/java.h"
#include "../../../json/json.hpp"
#include "../../../base.h"
#include "../../../../xorstr.h"
#include <windows.h>
#include <winhttp.h>
// connect to API pragma
#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

std::map<std::string, TabListStatsData> TabList::nameCache;
std::mutex TabList::cacheMutex;
std::queue<std::string> TabList::fetchQueue;
std::mutex TabList::queueMutex;
std::atomic<bool> TabList::threadRunning{ false };
std::thread TabList::workerThread;
bool s_injecting = false;
static bool s_tabWatermarkActive = false;

static bool clearJNI() {
	if (!Java::env) return false;
	if (Java::env->ExceptionCheck()) { Java::env->ExceptionClear(); return true; }
	return false;
}
// skips invalid names (not common on pika, gets rid of bedrock players and some bots)
static bool isValidName(const std::string& n) {
	if (n.length() < 3 || n.length() > 16) return false;
	for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
	return true;
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
// Minecraft tab list dosent support all colours, so if the user selects a RGB value in the ClickGUI that isnt supported, we simply get the closest colour possible
static std::string ClosestMCColor(float* rgb) {
	float r = rgb[0], g = rgb[1], b = rgb[2];
	struct MC { std::string c; float r, g, b; };
	static const MC c[] = {
		{"\xC2\xA7""0",0,0,0},{"\xC2\xA7""1",0,0,.66f},{"\xC2\xA7""2",0,.66f,0},{"\xC2\xA7""3",0,.66f,.66f},
		{"\xC2\xA7""4",.66f,0,0},{"\xC2\xA7""5",.66f,0,.66f},{"\xC2\xA7""6",1,.66f,0},{"\xC2\xA7""7",.66f,.66f,.66f},
		{"\xC2\xA7""8",.33f,.33f,.33f},{"\xC2\xA7""9",.33f,.33f,1},{"\xC2\xA7""a",.33f,1,.33f},{"\xC2\xA7""b",.33f,1,1},
		{"\xC2\xA7""c",1,.33f,.33f},{"\xC2\xA7""d",1,.33f,1},{"\xC2\xA7""e",1,1,.33f},{"\xC2\xA7""f",1,1,1}
	};
	std::string best = "\xC2\xA7""7"; float md = 9999;
	for (auto& x : c) { float d = (r - x.r)*(r - x.r) + (g - x.g)*(g - x.g) + (b - x.b)*(b - x.b); if (d < md) { md = d; best = x.c; } }
	return best;
}

static bool hasMatchingKeyword(const std::string& clean) {
	for (const auto& kw : settings::AntiSpam_Keywords) {
		std::string kwLower;
		for (char c : kw) kwLower += (char)tolower((unsigned char)c);
		if (clean.find(kwLower) != std::string::npos) return true;
	}
	return false;
}

static void stripSectionAndLower(const std::string& in, std::string& out) {
	out.clear();
	for (size_t i = 0; i < in.size(); ) {
		if ((unsigned char)in[i] == 0xC2 && i + 1 < in.size() && (unsigned char)in[i+1] == 0xA7) {
			i += 2; continue;
		}
		out += (char)tolower((unsigned char)in[i]);
		i++;
	}
}
// attempts to block pikas sale message blocking full screen (e.g. Sumer Sale 60% off), generally dosent work yet duo to mappings
static void suppressTitleSpam() {
	if (!settings::AntiSpam_Enabled) return;
	if (!Java::env) return;
	if (!StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft || !StrayCache::minecraft_ingameGUI) return;

	jobject mc = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
	if (clearJNI() || !mc) return;

	jobject guiObj = Java::env->GetObjectField(mc, StrayCache::minecraft_ingameGUI);
	if (clearJNI() || !guiObj) { Java::env->DeleteLocalRef(mc); return; }

	jclass guiClass = Java::env->GetObjectClass(guiObj);

	static jfieldID displayedTitleFid = nullptr;
	static jfieldID displayedSubTitleFid = nullptr;
	static jfieldID titleDisplayTicksFid = nullptr;
	static bool searchedFields = false;

	if (!searchedFields) {
		searchedFields = true;
		displayedTitleFid = Java::env->GetFieldID(guiClass, "field_175201_x", "Ljava/lang/String;");
		if (!displayedTitleFid) { clearJNI(); displayedTitleFid = Java::env->GetFieldID(guiClass, "displayedTitle", "Ljava/lang/String;"); }
		if (!displayedTitleFid) { clearJNI(); displayedTitleFid = Java::env->GetFieldID(guiClass, "w", "Ljava/lang/String;"); }

		displayedSubTitleFid = Java::env->GetFieldID(guiClass, "field_175200_y", "Ljava/lang/String;");
		if (!displayedSubTitleFid) { clearJNI(); displayedSubTitleFid = Java::env->GetFieldID(guiClass, "displayedSubTitle", "Ljava/lang/String;"); }
		if (!displayedSubTitleFid) { clearJNI(); displayedSubTitleFid = Java::env->GetFieldID(guiClass, "x", "Ljava/lang/String;"); }

		titleDisplayTicksFid = Java::env->GetFieldID(guiClass, "field_175199_z", "I");
		if (!titleDisplayTicksFid) { clearJNI(); titleDisplayTicksFid = Java::env->GetFieldID(guiClass, "titlesTimer", "I"); }
		if (!titleDisplayTicksFid) { clearJNI(); titleDisplayTicksFid = Java::env->GetFieldID(guiClass, "r", "I"); }
	}

	std::string titleClean, subClean;
	bool hasTitle = false, hasSub = false;

	if (displayedTitleFid) {
		jstring title = (jstring)Java::env->GetObjectField(guiObj, displayedTitleFid);
		if (!clearJNI() && title) {
			const char* tc = Java::env->GetStringUTFChars(title, nullptr);
			if (tc) { std::string ts(tc); Java::env->ReleaseStringUTFChars(title, tc); stripSectionAndLower(ts, titleClean); hasTitle = true; }
			Java::env->DeleteLocalRef(title);
		}
	}
	if (displayedSubTitleFid) {
		jstring sub = (jstring)Java::env->GetObjectField(guiObj, displayedSubTitleFid);
		if (!clearJNI() && sub) {
			const char* sc = Java::env->GetStringUTFChars(sub, nullptr);
			if (sc) { std::string ss(sc); Java::env->ReleaseStringUTFChars(sub, sc); stripSectionAndLower(ss, subClean); hasSub = true; }
			Java::env->DeleteLocalRef(sub);
		}
	}

	if (!hasTitle && !hasSub) {
		Java::env->DeleteLocalRef(guiClass); Java::env->DeleteLocalRef(guiObj); Java::env->DeleteLocalRef(mc);
		return;
	}

	bool shouldClear = false;
	if (hasTitle && hasMatchingKeyword(titleClean)) shouldClear = true;
	if (!shouldClear && hasSub && hasMatchingKeyword(subClean)) shouldClear = true;

	if (shouldClear) {
		jstring emptyStr = Java::env->NewStringUTF("");
		if (emptyStr) {
			if (displayedTitleFid) Java::env->SetObjectField(guiObj, displayedTitleFid, emptyStr);
			if (displayedSubTitleFid) Java::env->SetObjectField(guiObj, displayedSubTitleFid, emptyStr);
			Java::env->DeleteLocalRef(emptyStr);
		}
		if (titleDisplayTicksFid) {
			Java::env->SetIntField(guiObj, titleDisplayTicksFid, 0);
		}
		clearJNI();
	}

	Java::env->DeleteLocalRef(guiClass);
	Java::env->DeleteLocalRef(guiObj);
	Java::env->DeleteLocalRef(mc);
}
// creates the sufix in tab list for appending stats (it must be appended, not replaced)
std::string TabList::BuildStatSuffix(const TabListStatsData& s) {
	if (!s.isValid) return "";
	if (!s.fetchCompleted) return "";
	if (s.isNicked) return " \xC2\xA7""c[NICK]";
	if (s.isStatsOff) return " \xC2\xA7""c[OFF]";
	if (!s.hasValidStats) return " \xC2\xA7""7[N/A]";
	bool isZeroStats = (s.level <= 1 && s.finalKills == 0 && s.kills == 0 && s.wins == 0 && s.bedsBroken == 0 && s.winstreak == 0);
	if (isZeroStats) return " \xC2\xA7""7[N/A]";
	auto tr = [](float v) {
		if (!settings::TL_UseThresholdColors) return std::string("");
		if (v >= settings::TL_Thresh_Ratio_High) return ClosestMCColor(settings::TL_Col_Ratio_High);
		if (v >= settings::TL_Thresh_Ratio_Med)  return ClosestMCColor(settings::TL_Col_Ratio_Med);
		if (v >= settings::TL_Thresh_Ratio_Low)  return ClosestMCColor(settings::TL_Col_Ratio_Low);
		return ClosestMCColor(settings::TL_Col_Ratio_Def);
	};
	auto tw = [](float v) {
		if (!settings::TL_UseThresholdColors) return std::string("");
		if (v >= settings::TL_Thresh_WS_High) return ClosestMCColor(settings::TL_Col_WS_High);
		if (v >= settings::TL_Thresh_WS_Med)  return ClosestMCColor(settings::TL_Col_WS_Med);
		if (v >= settings::TL_Thresh_WS_Low)  return ClosestMCColor(settings::TL_Col_WS_Low);
		return ClosestMCColor(settings::TL_Col_WS_Def);
	};
	if (settings::TL_FormatMode == 0) {
		std::string r;
		auto a = [&](bool sp, std::string st) { if (sp && !r.empty()) r += " "; r += st; };
		if (settings::TL_showLevel) a(true, ClosestMCColor(settings::TL_Col_Level) + "L" + std::to_string(s.level));
		if (settings::TL_showFkdr) { char b[24]; snprintf(b, 24, "%.2f", s.fkdr); a(true, ClosestMCColor(settings::TL_Col_Pref_Fkdr) + "F" + tr(s.fkdr) + b); }
		if (settings::TL_showKdr) { char b[24]; snprintf(b, 24, "%.2f", s.kdr); a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "K" + tr(s.kdr) + b); }
		if (settings::TL_showWlr) { char b[24]; snprintf(b, 24, "%.2f", s.wlr); a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "WLR " + tr(s.wlr) + b); }
		if (settings::TL_showWS) { a(true, ClosestMCColor(settings::TL_Col_Pref_WS) + "WS" + tw((float)s.winstreak) + std::to_string(s.winstreak)); }
		if (settings::TL_showWins) a(true, ClosestMCColor(settings::TL_Col_Wins) + "W" + std::to_string(s.wins));
		if (settings::TL_showLosses) a(true, ClosestMCColor(settings::TL_Col_Losses) + "L" + std::to_string(s.losses));
		if (settings::TL_showBeds) a(true, ClosestMCColor(settings::TL_Col_Beds) + "B" + std::to_string(s.bedsBroken));
		if (settings::TL_showGames) a(true, ClosestMCColor(settings::TL_Col_Games) + "G" + std::to_string(s.gamesPlayed));
		if (settings::TL_showFinalKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Fkdr) + "FK " + std::to_string(s.finalKills));
		if (settings::TL_showFinalDeaths) a(true, ClosestMCColor(settings::TL_Col_Losses) + "FD " + std::to_string(s.finalDeaths));
		if (settings::TL_showKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "K " + std::to_string(s.kills));
		if (settings::TL_showDeaths) a(true, ClosestMCColor(settings::TL_Col_Losses) + "D " + std::to_string(s.deaths));
		if (settings::TL_showArrows) a(true, ClosestMCColor(settings::TL_Col_Arrows) + "A" + std::to_string(s.arrowsShot));
		if (settings::TL_showArrowsHit) a(true, ClosestMCColor(settings::TL_Col_Arrows) + "AH " + std::to_string(s.arrowsHit));
		if (settings::TL_showMissedShots) a(true, ClosestMCColor(settings::TL_Col_Losses) + "MS " + std::to_string(s.missedShots));
		if (settings::TL_showBowKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "BK " + std::to_string(s.bowKills));
		if (settings::TL_showVoidKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "VK " + std::to_string(s.voidKills));
		if (settings::TL_showMeleeKills) a(true, ClosestMCColor(settings::TL_Col_Pref_Kdr) + "MK " + std::to_string(s.meleeKills));
		return " " + r;
	}
	else {
		std::string res = settings::TL_FormatString;
		auto rp = [](std::string& s, const std::string& f, const std::string& t) { size_t p = 0; while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.length(), t); p += t.length(); } };
		char b[24];
		snprintf(b, 24, "%.2f", s.fkdr); rp(res, "{fkdr}", b);
		snprintf(b, 24, "%.2f", s.kdr);  rp(res, "{kdr}", b);
		snprintf(b, 24, "%.2f", s.wlr);  rp(res, "{wlr}", b);
		rp(res, "{level}", std::to_string(s.level)); rp(res, "{winstreak}", std::to_string(s.winstreak));
		rp(res, "{wins}", std::to_string(s.wins)); rp(res, "{losses}", std::to_string(s.losses));
		rp(res, "{games}", std::to_string(s.gamesPlayed)); rp(res, "{beds}", std::to_string(s.bedsBroken));
		rp(res, "{finalKills}", std::to_string(s.finalKills)); rp(res, "{finalDeaths}", std::to_string(s.finalDeaths));
		rp(res, "{kills}", std::to_string(s.kills)); rp(res, "{deaths}", std::to_string(s.deaths));
		rp(res, "&&", "\xC2\xA7");
		return " " + res;
	}
}

TabList::TabList() { threadRunning = true; workerThread = std::thread(&TabList::WorkerThreadFunc); }
TabList::~TabList() { threadRunning = false; if (workerThread.joinable()) workerThread.join(); }
bool TabList::IsEnabled() { return settings::TL_Enabled; }
void TabList::SetEnabled(bool enabled) {
	if (settings::TL_Enabled == enabled) return;
	settings::TL_Enabled = enabled;
	if (!enabled) s_injecting = false;
}
void TabList::Toggle() { SetEnabled(!settings::TL_Enabled); }
void TabList::ClearCache() { std::lock_guard<std::mutex> l(cacheMutex); nameCache.clear(); }

void TabList::ClearAllDisplayNames() {
	if (!Java::env || !StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft) return;
	if (!StrayCache::minecraft_getNetHandler) return;
	if (!StrayCache::netHandlerPlayClient_getPlayerInfoMap && !StrayCache::netHandlerPlayClient_playerInfoList) return;
	if (!StrayCache::networkPlayerInfo_setDisplayName) return;
	if (!StrayCache::collection_iterator || !StrayCache::iterator_hasNext || !StrayCache::iterator_next) return;
	try {
		jobject mc = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
		if (clearJNI() || !mc) return;
		jobject nh = Java::env->CallObjectMethod(mc, StrayCache::minecraft_getNetHandler);
		if (clearJNI() || !nh) { Java::env->DeleteLocalRef(mc); return; }
		jobject col = nullptr;
		if (StrayCache::netHandlerPlayClient_playerInfoList && StrayCache::map_values) {
			jobject map = Java::env->GetObjectField(nh, StrayCache::netHandlerPlayClient_playerInfoList);
			if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, StrayCache::map_values); Java::env->DeleteLocalRef(map); }
		}
		if (!col && StrayCache::netHandlerPlayClient_getPlayerInfoMap) {
			col = Java::env->CallObjectMethod(nh, StrayCache::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
		}
		if (col) {
			jobject it = Java::env->CallObjectMethod(col, StrayCache::collection_iterator);
			if (!clearJNI() && it) {
				int n = 0;
				while (n++ < 1000) {
					if (!Java::env->CallBooleanMethod(it, StrayCache::iterator_hasNext)) break;
					if (clearJNI()) break;
					jobject pi = Java::env->CallObjectMethod(it, StrayCache::iterator_next);
					if (clearJNI()) break;
					if (pi) {
						Java::env->CallVoidMethod(pi, StrayCache::networkPlayerInfo_setDisplayName, nullptr);
						clearJNI();
						Java::env->DeleteLocalRef(pi);
					}
				}
				Java::env->DeleteLocalRef(it);
			}
			clearJNI();
			Java::env->DeleteLocalRef(col);
		}
		Java::env->DeleteLocalRef(nh);
		Java::env->DeleteLocalRef(mc);
	}
	catch (...) {}
	clearJNI();
	s_injecting = false;
}
// actually injects the stats after building them, while keeping safe limits to prevent crashes
void TabList::InjectAllTabStats() {
	if (!Java::env) return;
	if (!StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft || !StrayCache::minecraft_getNetHandler) return;
	if (!StrayCache::netHandlerPlayClient_getPlayerInfoMap && !StrayCache::netHandlerPlayClient_playerInfoList) return;
	if (!StrayCache::networkPlayerInfo_setDisplayName || !StrayCache::networkPlayerInfo_getGameProfile) return;
	if (!StrayCache::gameProfile_class || !StrayCache::gameProfile_getName) return;
	if (!StrayCache::chatComponentText_class || !StrayCache::chatComponentText_init) return;
	if (!StrayCache::map_values || !StrayCache::collection_iterator || !StrayCache::iterator_hasNext || !StrayCache::iterator_next) return;
	bool canReadDisplay = (StrayCache::networkPlayerInfo_getDisplayName &&
		StrayCache::ichatcomponent_class && StrayCache::ichatcomponent_getFormattedText);
	const std::string MARKER = " \xC2\xA7r\xC2\xA7r ";
	int cnt = 0;
	try {
		jobject mc = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
		if (clearJNI() || !mc) return;
		jobject nh = Java::env->CallObjectMethod(mc, StrayCache::minecraft_getNetHandler);
		if (clearJNI() || !nh) { Java::env->DeleteLocalRef(mc); return; }
		jobject col = nullptr;
		if (StrayCache::netHandlerPlayClient_playerInfoList && StrayCache::map_values) {
			jobject map = Java::env->GetObjectField(nh, StrayCache::netHandlerPlayClient_playerInfoList);
			if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, StrayCache::map_values); Java::env->DeleteLocalRef(map); }
		}
		if (!col && StrayCache::netHandlerPlayClient_getPlayerInfoMap) {
			col = Java::env->CallObjectMethod(nh, StrayCache::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
		}
		if (!col) { Java::env->DeleteLocalRef(nh); Java::env->DeleteLocalRef(mc); return; }
		jobject it = Java::env->CallObjectMethod(col, StrayCache::collection_iterator);
		if (clearJNI() || !it) { Java::env->DeleteLocalRef(col); Java::env->DeleteLocalRef(nh); Java::env->DeleteLocalRef(mc); return; }
		int limit = 0;
		while (limit++ < 1000) {
			if (!Java::env->CallBooleanMethod(it, StrayCache::iterator_hasNext)) break;
			if (clearJNI()) break;
			jobject pi = Java::env->CallObjectMethod(it, StrayCache::iterator_next);
			if (clearJNI()) break;
			if (!pi) continue;
			std::string rawName;
			jobject gp = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getGameProfile);
			if (!clearJNI() && gp) {
				jstring nj = (jstring)Java::env->CallObjectMethod(gp, StrayCache::gameProfile_getName);
				if (!clearJNI() && nj) {
					const char* c = Java::env->GetStringUTFChars(nj, nullptr);
					if (c) { rawName = c; Java::env->ReleaseStringUTFChars(nj, c); }
					Java::env->DeleteLocalRef(nj);
				}
				Java::env->DeleteLocalRef(gp);
			}
			if (!isValidName(rawName)) { Java::env->DeleteLocalRef(pi); continue; }
			std::string baseName = rawName;
			bool handledBaseName = false;
			if (canReadDisplay) {
				jobject displayComp = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getDisplayName);
				if (!clearJNI() && displayComp != nullptr) {
					jstring df = (jstring)Java::env->CallObjectMethod(displayComp, StrayCache::ichatcomponent_getFormattedText);
					if (!clearJNI() && df != nullptr) {
						const char* dc = Java::env->GetStringUTFChars(df, nullptr);
						if (dc != nullptr) {
							std::string existingDisp = dc;
							Java::env->ReleaseStringUTFChars(df, dc);
							if (!existingDisp.empty() && existingDisp.find(MARKER) == std::string::npos) {
								baseName = existingDisp;
								handledBaseName = true;
							} else if (existingDisp.find(MARKER) != std::string::npos) {
								baseName = existingDisp.substr(0, existingDisp.find(MARKER));
								handledBaseName = true;
							}
						}
						Java::env->DeleteLocalRef(df);
					}
					Java::env->DeleteLocalRef(displayComp);
				}
				clearJNI();
			}
			if (!handledBaseName && StrayCache::networkPlayerInfo_getPlayerTeam && StrayCache::scorePlayerTeam_getColorPrefix && StrayCache::scorePlayerTeam_getColorSuffix) {
				jobject teamObj = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getPlayerTeam);
				if (!clearJNI() && teamObj != nullptr) {
					std::string prefixStr = "";
					std::string suffixStr = "";
					jstring pStr = (jstring)Java::env->CallObjectMethod(teamObj, StrayCache::scorePlayerTeam_getColorPrefix);
					if (!clearJNI() && pStr) {
						const char* pChars = Java::env->GetStringUTFChars(pStr, nullptr);
						if (pChars) { prefixStr = pChars; Java::env->ReleaseStringUTFChars(pStr, pChars); }
						Java::env->DeleteLocalRef(pStr);
					}
					jstring sStr = (jstring)Java::env->CallObjectMethod(teamObj, StrayCache::scorePlayerTeam_getColorSuffix);
					if (!clearJNI() && sStr) {
						const char* sChars = Java::env->GetStringUTFChars(sStr, nullptr);
						if (sChars) { suffixStr = sChars; Java::env->ReleaseStringUTFChars(sStr, sChars); }
						Java::env->DeleteLocalRef(sStr);
					}
					baseName = prefixStr + rawName + suffixStr;
					handledBaseName = true;
					Java::env->DeleteLocalRef(teamObj);
				}
				clearJNI();
			}
			TabListStatsData st; bool hs = false;
			{ std::lock_guard<std::mutex> l(cacheMutex); auto f = nameCache.find(rawName); if (f != nameCache.end()) { st = f->second; hs = true; } }
			if (!hs) { Java::env->DeleteLocalRef(pi); continue; }
			std::string suf = BuildStatSuffix(st);
			if (suf.empty()) { Java::env->DeleteLocalRef(pi); continue; }
			std::string nd = baseName + MARKER + suf;
			jstring nj = Java::env->NewStringUTF(nd.c_str());
			if (nj) {
				jobject cc = Java::env->NewObject(StrayCache::chatComponentText_class, StrayCache::chatComponentText_init, nj);
				if (!clearJNI() && cc) {
					Java::env->CallVoidMethod(pi, StrayCache::networkPlayerInfo_setDisplayName, cc);
					clearJNI();
					Java::env->DeleteLocalRef(cc);
					cnt++;
				}
				Java::env->DeleteLocalRef(nj);
			}
			Java::env->DeleteLocalRef(pi);
		}
		Java::env->DeleteLocalRef(it);
		Java::env->DeleteLocalRef(col);
		Java::env->DeleteLocalRef(nh);
		Java::env->DeleteLocalRef(mc);
	}
	catch (...) {}
	clearJNI();
	if (cnt > 0) s_injecting = true;
}
// makes sure to not display stats when the user is not in the correct gamemode or server and lets you choose if you want to use the tab list injection in lobby, or only in game (checked by hasMap)
bool TabList::ShouldShowStats(jobject mc) {
	if (!settings::TL_Enabled || !mc || !Java::env) return false;
	if (!StrayCache::minecraft_theWorld) return true;
	jobject world = Java::env->GetObjectField(mc, StrayCache::minecraft_theWorld);
	if (clearJNI() || !world) return false;
	if (!StrayCache::world_getScoreboard || !StrayCache::scoreboard_getObjectiveInDisplaySlot || !StrayCache::scoreObjective_getDisplayName) {
		Java::env->DeleteLocalRef(world);
		return true;
	}
	bool hasBedwars = false;
	bool hasMap = false;
	bool hasPika = false;
	jobject sb = Java::env->CallObjectMethod(world, StrayCache::world_getScoreboard);
	if (clearJNI() || !sb) { Java::env->DeleteLocalRef(world); return true; }
	jobject obj = Java::env->CallObjectMethod(sb, StrayCache::scoreboard_getObjectiveInDisplaySlot, 1);
	if (!clearJNI() && obj) {
		jstring ds = (jstring)Java::env->CallObjectMethod(obj, StrayCache::scoreObjective_getDisplayName);
		if (!clearJNI() && ds) {
			const char* c = Java::env->GetStringUTFChars(ds, nullptr);
			if (c) {
				std::string dn = c; Java::env->ReleaseStringUTFChars(ds, c);
				for (auto& x : dn) x = (char)toupper((unsigned char)x);
				if (dn.find("BEDWARS") != std::string::npos) hasBedwars = true;
			}
			Java::env->DeleteLocalRef(ds);
		}
		if (hasBedwars && StrayCache::scoreboard_getSortedScores && StrayCache::score_getPlayerName && StrayCache::scoreboard_getPlayersTeam && StrayCache::scorePlayerTeam_formatPlayerName && StrayCache::collection_iterator && StrayCache::iterator_hasNext && StrayCache::iterator_next) {
			jobject scoresCol = Java::env->CallObjectMethod(sb, StrayCache::scoreboard_getSortedScores, obj);
			if (!clearJNI() && scoresCol) {
				jobject it = Java::env->CallObjectMethod(scoresCol, StrayCache::collection_iterator);
				if (!clearJNI() && it) {
					while (Java::env->CallBooleanMethod(it, StrayCache::iterator_hasNext)) {
						if (clearJNI()) break;
						jobject scoreObj = Java::env->CallObjectMethod(it, StrayCache::iterator_next);
						if (clearJNI()) break;
						if (scoreObj) {
							jstring pNameJ = (jstring)Java::env->CallObjectMethod(scoreObj, StrayCache::score_getPlayerName);
							if (!clearJNI() && pNameJ) {
								jobject teamObj = Java::env->CallObjectMethod(sb, StrayCache::scoreboard_getPlayersTeam, pNameJ);
								bool mustDeleteFormatted = false;
								jstring formattedNameJ = nullptr;
								if (!clearJNI()) {
									if (teamObj) {
										formattedNameJ = (jstring)Java::env->CallStaticObjectMethod(StrayCache::scorePlayerTeam_class, StrayCache::scorePlayerTeam_formatPlayerName, teamObj, pNameJ);
										mustDeleteFormatted = true;
									} else { formattedNameJ = pNameJ; }
									if (!clearJNI() && formattedNameJ) {
										const char* cStr = Java::env->GetStringUTFChars(formattedNameJ, nullptr);
										if (cStr) {
											std::string line = cStr; Java::env->ReleaseStringUTFChars(formattedNameJ, cStr);
											for (auto& x : line) x = (char)toupper((unsigned char)x);
											if (line.find("MAP") != std::string::npos) hasMap = true;
											if (line.find("PIKA") != std::string::npos) hasPika = true;
										}
										if (mustDeleteFormatted) Java::env->DeleteLocalRef(formattedNameJ);
									}
									if (teamObj) Java::env->DeleteLocalRef(teamObj);
								}
								Java::env->DeleteLocalRef(pNameJ);
							}
							Java::env->DeleteLocalRef(scoreObj);
						}
						if (hasMap) break;
					}
					Java::env->DeleteLocalRef(it);
				}
				Java::env->DeleteLocalRef(scoresCol);
			}
		}
		Java::env->DeleteLocalRef(obj);
	}
	Java::env->DeleteLocalRef(sb);
	Java::env->DeleteLocalRef(world);
	clearJNI();
	if (!hasBedwars || !hasPika) return false;
	bool isLobby = !hasMap;
	if (settings::TL_HideInLobby && isLobby) return false;
	return true;
}
// injects tab list watermark in the footer (replaces the sale spam), works only on forge (cache needs mappings fixed for vanila), even tho it dosent work on vanila it works on lunar 1.8.9 optifine for some reason
void TabList::InjectTabWatermarkFooter() {
	if (!Java::env) return;
	if (!StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft) return;
	if (!StrayCache::minecraft_ingameGUI) return;
	if (!StrayCache::chatComponentText_class || !StrayCache::chatComponentText_init) return;

	try {
		jobject mcObj = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
		if (clearJNI() || !mcObj) return;

		jobject ingameGUI = Java::env->GetObjectField(mcObj, StrayCache::minecraft_ingameGUI);
		Java::env->DeleteLocalRef(mcObj);
		if (clearJNI() || !ingameGUI) return;

		jobject tabOverlay = nullptr;
		if (StrayCache::guiIngame_overlayPlayerList) {
			jclass guiIngameClass = Java::env->GetObjectClass(ingameGUI);
			if (!clearJNI() && guiIngameClass) {
				tabOverlay = Java::env->GetObjectField(ingameGUI, StrayCache::guiIngame_overlayPlayerList);
				Java::env->DeleteLocalRef(guiIngameClass);
			}
			clearJNI();
		}
		if (!tabOverlay && StrayCache::guiPlayerTabOverlay_class) {
			jclass guiIngameClass = Java::env->GetObjectClass(ingameGUI);
			if (!clearJNI() && guiIngameClass) {
				jfieldID tabListField = Java::env->GetFieldID(guiIngameClass, "field_175196_v", "Lnet/minecraft/client/gui/GuiPlayerTabOverlay;");
				if (!tabListField) { clearJNI(); tabListField = Java::env->GetFieldID(guiIngameClass, "overlayPlayerList", "Lnet/minecraft/client/gui/GuiPlayerTabOverlay;"); }
				if (!tabListField) { clearJNI(); tabListField = Java::env->GetFieldID(guiIngameClass, "q", "Lawh;"); }
				if (!tabListField) { clearJNI(); tabListField = Java::env->GetFieldID(guiIngameClass, "j", "Lawh;"); }
				if (tabListField) tabOverlay = Java::env->GetObjectField(ingameGUI, tabListField);
				Java::env->DeleteLocalRef(guiIngameClass);
			}
			clearJNI();
		}
		Java::env->DeleteLocalRef(ingameGUI);
		if (!tabOverlay) return;

		jfieldID footerField = StrayCache::guiPlayerTabOverlay_footer;
		if (!footerField) {
			jclass tabClass = Java::env->GetObjectClass(tabOverlay);
			if (!clearJNI() && tabClass) {
				footerField = Java::env->GetFieldID(tabClass, "field_175255_h", "Lnet/minecraft/util/IChatComponent;");
				if (!footerField) { clearJNI(); footerField = Java::env->GetFieldID(tabClass, "footer", "Lnet/minecraft/util/IChatComponent;"); }
				if (!footerField) { clearJNI(); footerField = Java::env->GetFieldID(tabClass, "i", "Leu;"); }
				if (!footerField) { clearJNI(); footerField = Java::env->GetFieldID(tabClass, "h", "Leu;"); }
				Java::env->DeleteLocalRef(tabClass);
			}
			clearJNI();
			if (!footerField) { Java::env->DeleteLocalRef(tabOverlay); return; }
		}

		jstring wmStr = Java::env->NewStringUTF("\xC2\xA7""d\xC2\xA7""lCutie \xC2\xA7""8\xC2\xBB \xC2\xA7""bdiscord: \xC2\xA7""dlinixie.");
		if (!wmStr) { Java::env->DeleteLocalRef(tabOverlay); return; }

		jobject chatComp = Java::env->NewObject(StrayCache::chatComponentText_class, StrayCache::chatComponentText_init, wmStr);
		Java::env->DeleteLocalRef(wmStr);
		if (clearJNI() || !chatComp) { Java::env->DeleteLocalRef(tabOverlay); return; }

		Java::env->SetObjectField(tabOverlay, footerField, chatComp);
		clearJNI();
		s_tabWatermarkActive = true;

		Java::env->DeleteLocalRef(chatComp);
		Java::env->DeleteLocalRef(tabOverlay);
	}
	catch (...) {
		clearJNI();
		s_tabWatermarkActive = false;
	}
}

void TabList::ClearTabWatermarkFooter() {
	if (!s_tabWatermarkActive) return;
	if (!Java::env) return;
	if (!StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft) return;
	if (!StrayCache::minecraft_ingameGUI) return;

	try {
		jobject mcObj = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
		if (clearJNI() || !mcObj) return;

		jobject ingameGUI = Java::env->GetObjectField(mcObj, StrayCache::minecraft_ingameGUI);
		Java::env->DeleteLocalRef(mcObj);
		if (clearJNI() || !ingameGUI) return;

		jobject tabOverlay = nullptr;
		if (StrayCache::guiIngame_overlayPlayerList) {
			jclass guiIngameClass = Java::env->GetObjectClass(ingameGUI);
			if (!clearJNI() && guiIngameClass) {
				tabOverlay = Java::env->GetObjectField(ingameGUI, StrayCache::guiIngame_overlayPlayerList);
				Java::env->DeleteLocalRef(guiIngameClass);
			}
			clearJNI();
		}
		if (!tabOverlay) {
			jclass guiIngameClass = Java::env->GetObjectClass(ingameGUI);
			if (!clearJNI() && guiIngameClass) {
				jfieldID tabListField = Java::env->GetFieldID(guiIngameClass, "field_175196_v", "Lnet/minecraft/client/gui/GuiPlayerTabOverlay;");
				if (!tabListField) { clearJNI(); tabListField = Java::env->GetFieldID(guiIngameClass, "overlayPlayerList", "Lnet/minecraft/client/gui/GuiPlayerTabOverlay;"); }
				if (!tabListField) { clearJNI(); tabListField = Java::env->GetFieldID(guiIngameClass, "q", "Lawh;"); }
				if (!tabListField) { clearJNI(); tabListField = Java::env->GetFieldID(guiIngameClass, "j", "Lawh;"); }
				if (tabListField) tabOverlay = Java::env->GetObjectField(ingameGUI, tabListField);
				Java::env->DeleteLocalRef(guiIngameClass);
			}
			clearJNI();
		}
		Java::env->DeleteLocalRef(ingameGUI);
		if (!tabOverlay) { s_tabWatermarkActive = false; return; }

		jfieldID footerField = StrayCache::guiPlayerTabOverlay_footer;
		if (!footerField) {
			jclass tabClass = Java::env->GetObjectClass(tabOverlay);
			if (!clearJNI() && tabClass) {
				footerField = Java::env->GetFieldID(tabClass, "field_175255_h", "Lnet/minecraft/util/IChatComponent;");
				if (!footerField) { clearJNI(); footerField = Java::env->GetFieldID(tabClass, "footer", "Lnet/minecraft/util/IChatComponent;"); }
				if (!footerField) { clearJNI(); footerField = Java::env->GetFieldID(tabClass, "i", "Leu;"); }
				if (!footerField) { clearJNI(); footerField = Java::env->GetFieldID(tabClass, "h", "Leu;"); }
				Java::env->DeleteLocalRef(tabClass);
			}
			clearJNI();
		}

		if (footerField) {
			Java::env->SetObjectField(tabOverlay, footerField, nullptr);
			clearJNI();
		}
		s_tabWatermarkActive = false;
		Java::env->DeleteLocalRef(tabOverlay);
	}
	catch (...) {
		clearJNI();
		s_tabWatermarkActive = false;
	}
}

void TabList::Update() {
	if (!settings::TL_Enabled) { if (s_injecting) ClearAllDisplayNames(); if (s_tabWatermarkActive) ClearTabWatermarkFooter(); return; }
	if (!Java::env || !StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft) return;
	jobject mc = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
	if (clearJNI() || !mc) return;
	if (!StrayCache::minecraft_theWorld) { Java::env->DeleteLocalRef(mc); return; }
	jobject world = Java::env->GetObjectField(mc, StrayCache::minecraft_theWorld);
	if (clearJNI() || !world) { if (s_injecting) ClearAllDisplayNames(); Java::env->DeleteLocalRef(mc); return; }
	bool hasBedwars = false;
	bool hasMap = false;
	bool hasPika = false;
	if (StrayCache::world_getScoreboard && StrayCache::scoreboard_getObjectiveInDisplaySlot && StrayCache::scoreObjective_getDisplayName) {
		jobject sb = Java::env->CallObjectMethod(world, StrayCache::world_getScoreboard);
		if (!clearJNI() && sb) {
			jobject obj = Java::env->CallObjectMethod(sb, StrayCache::scoreboard_getObjectiveInDisplaySlot, 1);
			if (!clearJNI() && obj) {
				jstring ds = (jstring)Java::env->CallObjectMethod(obj, StrayCache::scoreObjective_getDisplayName);
				if (!clearJNI() && ds) {
					const char* c = Java::env->GetStringUTFChars(ds, nullptr);
					if (c) {
						std::string dn = c; Java::env->ReleaseStringUTFChars(ds, c);
						for (auto& x : dn) x = (char)toupper((unsigned char)x);
						if (dn.find("BEDWARS") != std::string::npos) hasBedwars = true;
					}
					Java::env->DeleteLocalRef(ds);
				}
				if (hasBedwars && StrayCache::scoreboard_getSortedScores && StrayCache::score_getPlayerName && StrayCache::scoreboard_getPlayersTeam && StrayCache::scorePlayerTeam_formatPlayerName && StrayCache::collection_iterator && StrayCache::iterator_hasNext && StrayCache::iterator_next) {
					jobject scoresCol = Java::env->CallObjectMethod(sb, StrayCache::scoreboard_getSortedScores, obj);
					if (!clearJNI() && scoresCol) {
						jobject it = Java::env->CallObjectMethod(scoresCol, StrayCache::collection_iterator);
						if (!clearJNI() && it) {
							while (Java::env->CallBooleanMethod(it, StrayCache::iterator_hasNext)) {
								if (clearJNI()) break;
								jobject scoreObj = Java::env->CallObjectMethod(it, StrayCache::iterator_next);
								if (clearJNI()) break;
								if (scoreObj) {
									jstring pNameJ = (jstring)Java::env->CallObjectMethod(scoreObj, StrayCache::score_getPlayerName);
									if (!clearJNI() && pNameJ) {
										jobject teamObj = Java::env->CallObjectMethod(sb, StrayCache::scoreboard_getPlayersTeam, pNameJ);
										bool mustDeleteFormatted = false;
										jstring formattedNameJ = nullptr;
										if (!clearJNI()) {
											if (teamObj) { formattedNameJ = (jstring)Java::env->CallStaticObjectMethod(StrayCache::scorePlayerTeam_class, StrayCache::scorePlayerTeam_formatPlayerName, teamObj, pNameJ); mustDeleteFormatted = true; }
											else { formattedNameJ = pNameJ; }
											if (!clearJNI() && formattedNameJ) {
												const char* cStr = Java::env->GetStringUTFChars(formattedNameJ, nullptr);
												if (cStr) {
													std::string line = cStr; Java::env->ReleaseStringUTFChars(formattedNameJ, cStr);
													for (auto& x : line) x = (char)toupper((unsigned char)x);
													if (line.find("MAP") != std::string::npos) hasMap = true;
													if (line.find("PIKA") != std::string::npos) hasPika = true;
												}
												if (mustDeleteFormatted) Java::env->DeleteLocalRef(formattedNameJ);
											}
											if (teamObj) Java::env->DeleteLocalRef(teamObj);
										}
										Java::env->DeleteLocalRef(pNameJ);
									}
									Java::env->DeleteLocalRef(scoreObj);
								}
								if (hasMap) break;
							}
							Java::env->DeleteLocalRef(it);
						}
						Java::env->DeleteLocalRef(scoresCol);
					}
				}
				Java::env->DeleteLocalRef(obj);
			}
			clearJNI();
			Java::env->DeleteLocalRef(sb);
		}
		clearJNI();
	}
	static jobject lastWorld = nullptr;
	static bool lastInGame = false;
	static bool hasFetchedForCurrentState = false;
	bool worldChanged = false;
	if (lastWorld == nullptr || !Java::env->IsSameObject(lastWorld, world)) {
		worldChanged = true;
		if (lastWorld != nullptr) { Java::env->DeleteGlobalRef(lastWorld); }
		lastWorld = Java::env->NewGlobalRef(world);
	}
	bool inGame = hasMap;
	bool stateChanged = worldChanged || (inGame != lastInGame);
	lastInGame = inGame;
	if (stateChanged) {
		{ std::lock_guard<std::mutex> l(queueMutex); std::queue<std::string> e; std::swap(fetchQueue, e); }
		{ std::lock_guard<std::mutex> l(cacheMutex); nameCache.clear(); }
		hasFetchedForCurrentState = false;
	}
	Java::env->DeleteLocalRef(world);
	bool shouldShowStats = true;
	if (!hasBedwars || !hasPika) shouldShowStats = false;
	bool isLobby = !hasMap;
	if (settings::TL_HideInLobby && isLobby) shouldShowStats = false;
	if (!shouldShowStats) {
		if (s_injecting) ClearAllDisplayNames();
	}
	auto now = std::chrono::steady_clock::now();
	if (!hasFetchedForCurrentState) {
		jobject nh = Java::env->CallObjectMethod(mc, StrayCache::minecraft_getNetHandler);
		if (!clearJNI() && nh) {
			jobject col = nullptr;
			if (StrayCache::netHandlerPlayClient_playerInfoList && StrayCache::map_values) {
				jobject map = Java::env->GetObjectField(nh, StrayCache::netHandlerPlayClient_playerInfoList);
				if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, StrayCache::map_values); Java::env->DeleteLocalRef(map); }
			}
			if (!col && StrayCache::netHandlerPlayClient_getPlayerInfoMap) { col = Java::env->CallObjectMethod(nh, StrayCache::netHandlerPlayClient_getPlayerInfoMap); clearJNI(); }
			if (col && StrayCache::collection_iterator && StrayCache::iterator_hasNext && StrayCache::iterator_next) {
				jobject it = Java::env->CallObjectMethod(col, StrayCache::collection_iterator);
				if (!clearJNI() && it) {
					int n = 0;
					while (n++ < 1000) {
						if (!Java::env->CallBooleanMethod(it, StrayCache::iterator_hasNext)) break;
						if (clearJNI()) break;
						jobject pi = Java::env->CallObjectMethod(it, StrayCache::iterator_next);
						if (clearJNI()) break;
						if (pi) {
							jobject gp = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getGameProfile);
							if (!clearJNI() && gp) {
								jstring nj = (jstring)Java::env->CallObjectMethod(gp, StrayCache::gameProfile_getName);
								if (!clearJNI() && nj) {
									std::string nm;
									const char* c = Java::env->GetStringUTFChars(nj, nullptr);
									if (c) { nm = c; Java::env->ReleaseStringUTFChars(nj, c); }
									if (isValidName(nm)) {
										bool isNpc = false;
										if (StrayCache::networkPlayerInfo_getDisplayName && StrayCache::ichatcomponent_getFormattedText) {
											jobject dc2 = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getDisplayName);
											if (!clearJNI() && dc2 != nullptr) {
												jstring df2 = (jstring)Java::env->CallObjectMethod(dc2, StrayCache::ichatcomponent_getFormattedText);
												if (!clearJNI() && df2 != nullptr) {
													const char* dcc = Java::env->GetStringUTFChars(df2, nullptr);
													if (dcc != nullptr) {
														std::string ed = dcc; Java::env->ReleaseStringUTFChars(df2, dcc);
														for (auto& x : ed) x = (char)toupper((unsigned char)x);
														if (ed.find("NPC") != std::string::npos) isNpc = true;
													}
													Java::env->DeleteLocalRef(df2);
												}
												Java::env->DeleteLocalRef(dc2);
											}
											clearJNI();
										}
										if (!isNpc) {
											bool nf = true;
											{ std::lock_guard<std::mutex> lk(cacheMutex);
												auto cit = nameCache.find(nm);
												if (cit != nameCache.end()) {
													auto diff = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - cit->second.fetchTime).count();
													if (diff > 60) nameCache.erase(cit); else nf = false;
												}
												if (nf) {
													TabListStatsData nd;
													nd.isValid = true;
													nd.fetchTime = std::chrono::system_clock::now();
													nameCache[nm] = nd;
												}
											}
											if (nf) { std::lock_guard<std::mutex> lk2(queueMutex); fetchQueue.push(nm); }
										}
									}
									Java::env->DeleteLocalRef(nj);
								}
								Java::env->DeleteLocalRef(gp);
							}
							clearJNI();
							Java::env->DeleteLocalRef(pi);
						}
					}
					Java::env->DeleteLocalRef(it);
				}
				clearJNI();
			}
			if (col) Java::env->DeleteLocalRef(col);
			Java::env->DeleteLocalRef(nh);
		}
		clearJNI();
		hasFetchedForCurrentState = true;
	}
	if (!Base::isCompromised) { InjectTabWatermarkFooter(); }

	if (!shouldShowStats) {
		Java::env->DeleteLocalRef(mc);
		return;
	}
	static auto li = std::chrono::steady_clock::now();
	static auto lastRescan = std::chrono::steady_clock::now();
	if (std::chrono::duration_cast<std::chrono::milliseconds>(now - li).count() >= 500) {
		li = now;
		if (!Base::isCompromised) { InjectAllTabStats(); }
	}
	if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRescan).count() >= 2) {
		lastRescan = now;
		jobject nh2 = Java::env->CallObjectMethod(mc, StrayCache::minecraft_getNetHandler);
		if (!clearJNI() && nh2) {
			jobject col2 = nullptr;
			if (StrayCache::netHandlerPlayClient_playerInfoList && StrayCache::map_values) {
				jobject map2 = Java::env->GetObjectField(nh2, StrayCache::netHandlerPlayClient_playerInfoList);
				if (!clearJNI() && map2) { col2 = Java::env->CallObjectMethod(map2, StrayCache::map_values); Java::env->DeleteLocalRef(map2); }
			}
			if (!col2 && StrayCache::netHandlerPlayClient_getPlayerInfoMap) { col2 = Java::env->CallObjectMethod(nh2, StrayCache::netHandlerPlayClient_getPlayerInfoMap); clearJNI(); }
			if (col2 && StrayCache::collection_iterator && StrayCache::iterator_hasNext && StrayCache::iterator_next) {
				jobject it2 = Java::env->CallObjectMethod(col2, StrayCache::collection_iterator);
				if (!clearJNI() && it2) {
					int n2 = 0;
					while (n2++ < 1000) {
						if (!Java::env->CallBooleanMethod(it2, StrayCache::iterator_hasNext)) break;
						if (clearJNI()) break;
						jobject pi2 = Java::env->CallObjectMethod(it2, StrayCache::iterator_next);
						if (clearJNI() || !pi2) continue;
						jobject gp2 = Java::env->CallObjectMethod(pi2, StrayCache::networkPlayerInfo_getGameProfile);
						if (!clearJNI() && gp2) {
							jstring nj2 = (jstring)Java::env->CallObjectMethod(gp2, StrayCache::gameProfile_getName);
							if (!clearJNI() && nj2) {
								std::string nm2;
								const char* c2 = Java::env->GetStringUTFChars(nj2, nullptr);
								if (c2) { nm2 = c2; Java::env->ReleaseStringUTFChars(nj2, c2); }
								if (isValidName(nm2)) {
									bool needFetch = false;
									{ std::lock_guard<std::mutex> lk(cacheMutex);
										auto itc = nameCache.find(nm2);
										if (itc == nameCache.end()) {
											TabListStatsData nd;
											nd.isValid = true;
											nd.fetchTime = std::chrono::system_clock::now();
											nameCache[nm2] = nd;
											needFetch = true;
										}
										else if (!itc->second.fetchCompleted) {
											auto diff = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - itc->second.fetchTime).count();
											if (diff > 15) { itc->second.fetchTime = std::chrono::system_clock::now(); needFetch = true; }
										}
									}
									if (needFetch) { std::lock_guard<std::mutex> lk(queueMutex); fetchQueue.push(nm2); }
								}
								Java::env->DeleteLocalRef(nj2);
							}
							Java::env->DeleteLocalRef(gp2);
						}
						clearJNI();
						Java::env->DeleteLocalRef(pi2);
					}
					Java::env->DeleteLocalRef(it2);
				}
				clearJNI();
			}
			if (col2) Java::env->DeleteLocalRef(col2);
			Java::env->DeleteLocalRef(nh2);
		}
		clearJNI();
	}
	static auto lastTitleSuppress = std::chrono::steady_clock::now();
	if (settings::AntiSpam_Enabled && std::chrono::duration_cast<std::chrono::seconds>(now - lastTitleSuppress).count() >= 1) {
		lastTitleSuppress = now;
		suppressTitleSpam();
	}
	Java::env->DeleteLocalRef(mc);
}
// prevents doing everything in the main thread (it would cause FPS drops), so instead it makes new threads to not run on the main thread
void TabList::WorkerThreadFunc() {
	while (threadRunning) {
		std::vector<std::string> batch;
		{ std::lock_guard<std::mutex> l(queueMutex); while (!fetchQueue.empty() && batch.size() < 50) { batch.push_back(fetchQueue.front()); fetchQueue.pop(); } }
		if (!batch.empty()) {
			for (const auto& n : batch) FetchLeaderboardStats(n);
			for (const auto& n : batch) FetchProfileStats(n);
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
	}
}

std::string TabList::WinHttpGet(const std::string& url, int& sc) {
	std::string r; sc = 0;
	HINTERNET hS = WinHttpOpen(L"Cutie/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hS) return r;
	WinHttpSetTimeouts(hS, 5000, 5000, 5000, 5000);
	URL_COMPONENTS u; ZeroMemory(&u, sizeof(u)); u.dwStructSize = sizeof(u); u.dwHostNameLength = (DWORD)-1; u.dwUrlPathLength = (DWORD)-1;
	std::wstring w(url.begin(), url.end());
	if (WinHttpCrackUrl(w.c_str(), (DWORD)w.length(), 0, &u)) {
		std::wstring h(u.lpszHostName, u.dwHostNameLength), p(u.lpszUrlPath, u.dwUrlPathLength);
		HINTERNET hC = WinHttpConnect(hS, h.c_str(), u.nPort, 0);
		if (hC) {
			HINTERNET hR = WinHttpOpenRequest(hC, L"GET", p.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, (u.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
			if (hR) {
				if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hR, NULL)) {
					DWORD sz = sizeof(DWORD); WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sz, WINHTTP_NO_HEADER_INDEX);
					DWORD avail = 0; char b[4096];
					while (WinHttpQueryDataAvailable(hR, &avail) && avail > 0) { DWORD dn = 0; if (avail > sizeof(b)) avail = sizeof(b); if (WinHttpReadData(hR, b, avail, &dn) && dn > 0) r.append(b, dn); else break; }
				}
				WinHttpCloseHandle(hR);
			}
			WinHttpCloseHandle(hC);
		}
	}
	WinHttpCloseHandle(hS);
	return r;
}

void TabList::FetchLeaderboardStats(const std::string& name) {
	if (name.empty() || name.length() > 16) return;
	{ std::lock_guard<std::mutex> l(cacheMutex); auto it = nameCache.find(name); if (it != nameCache.end() && it->second.fetchCompleted) return; }
	TabListStatsData s; s.isValid = true; s.fetchTime = std::chrono::system_clock::now();
	static const char* intervalApiKeys[] = { "lifetime", "monthly", "weekly", "yearly" };
	static const char* modeApiKeys[] = { "SOLO", "DOUBLES", "QUADS", "ALL_MODES" };
	int lbCode = 0;
	std::string lbUrl = "https://stats.pika-network.net/api/profile/" + name + "/leaderboard?type=bedwars&interval=" + intervalApiKeys[settings::TL_Interval] + "&mode=" + modeApiKeys[settings::TL_Mode];
	std::string lbJson = WinHttpGet(lbUrl, lbCode);
	if (lbCode == 204 || (lbCode == 200 && lbJson.empty())) {
		s.isStatsOff = true;
		s.fetchCompleted = true;
		std::lock_guard<std::mutex> l(cacheMutex);
		auto it = nameCache.find(name);
		if (it != nameCache.end()) { s.level = it->second.level; s.clanTag = it->second.clanTag; }
		nameCache[name] = s;
		return;
	}
	if (lbCode == 404) {
		s.isNicked = true;
		s.fetchCompleted = true;
		std::lock_guard<std::mutex> l(cacheMutex);
		auto it = nameCache.find(name);
		if (it != nameCache.end()) { s.level = it->second.level; s.clanTag = it->second.clanTag; }
		nameCache[name] = s;
		return;
	}
	if (lbCode == 429) { { std::lock_guard<std::mutex> l(queueMutex); fetchQueue.push(name); } Sleep(1500); return; }
	if (lbCode == 200 && !lbJson.empty()) {
		try {
			json lb = json::parse(lbJson);
			bool hasAnyField = lb.contains("Final kills") || lb.contains("Kills") || lb.contains("Wins") || lb.contains("Beds destroyed") || lb.contains("Games played");
			if (lb.empty() || lb.contains("error") || !hasAnyField) {
				s.fetchCompleted = true;
			} else {
				s.hasValidStats = true;
				s.fetchCompleted = true;
				s.finalKills = ParseStatFromLB(lb, "Final kills"); s.finalDeaths = ParseStatFromLB(lb, "Final deaths");
				s.kills = ParseStatFromLB(lb, "Kills"); s.deaths = ParseStatFromLB(lb, "Deaths");
				s.wins = ParseStatFromLB(lb, "Wins"); s.losses = ParseStatFromLB(lb, "Losses");
				s.bedsBroken = ParseStatFromLB(lb, "Beds destroyed"); s.gamesPlayed = ParseStatFromLB(lb, "Games played");
				s.winstreak = ParseStatFromLB(lb, "Highest winstreak reached");
				s.arrowsShot = ParseStatFromLB(lb, "Arrows shot"); s.arrowsHit = ParseStatFromLB(lb, "Arrows hit");
				s.bowKills = ParseStatFromLB(lb, "Bow kills"); s.voidKills = ParseStatFromLB(lb, "Void kills"); s.meleeKills = ParseStatFromLB(lb, "Melee kills");
				s.missedShots = s.arrowsShot - s.arrowsHit; if (s.missedShots < 0) s.missedShots = 0;
				s.fkdr = s.finalDeaths > 0 ? (float)s.finalKills / s.finalDeaths : (float)s.finalKills;
				s.kdr = s.deaths > 0 ? (float)s.kills / s.deaths : (float)s.kills;
				s.wlr = s.losses > 0 ? (float)s.wins / s.losses : (float)s.wins;
			}
		} catch (...) {
			s.fetchCompleted = true;
		}
	}
	{ std::lock_guard<std::mutex> l(cacheMutex); auto it = nameCache.find(name); if (it != nameCache.end()) { s.level = it->second.level; s.clanTag = it->second.clanTag; } nameCache[name] = s; }
}
// for profile stats (200 empty means stats are off, but this only applys to the actual non profile stats, level can be extracted regardlessly)
void TabList::FetchProfileStats(const std::string& name) {
	if (name.empty() || name.length() > 16) return;
	int code = 0;
	std::string profileUrl = "https://stats.pika-network.net/api/profile/" + name;
	std::string profileJson = WinHttpGet(profileUrl, code);
	if (code == 404) {
		{ std::lock_guard<std::mutex> l(cacheMutex);
			auto it = nameCache.find(name);
			if (it == nameCache.end() || !it->second.fetchCompleted) {
				TabListStatsData s; s.isNicked = true; s.isValid = true; s.fetchCompleted = true; s.fetchTime = std::chrono::system_clock::now();
				nameCache[name] = s;
			}
		}
		return;
	}
	if (code == 429) { { std::lock_guard<std::mutex> l(queueMutex); fetchQueue.push(name); } Sleep(1500); return; }
	if (code != 200 || profileJson.empty()) return;
	std::string resolvedName = name;
	int level = 0;
	std::string clan;
	try {
		json p = json::parse(profileJson);
		if (p.contains("username") && p["username"].is_string()) resolvedName = p["username"].get<std::string>();
		if (p.contains("rank") && p["rank"].is_object() && p["rank"].contains("level")) level = p["rank"]["level"].get<int>();
		if (p.contains("clan") && p["clan"].is_object() && p["clan"].contains("tag")) clan = p["clan"]["tag"].get<std::string>();
	} catch (...) {}
	{
		std::lock_guard<std::mutex> l(cacheMutex);
		auto it = nameCache.find(name);
		if (it != nameCache.end() && it->second.isValid) { it->second.level = level; it->second.clanTag = clan; }
		if (resolvedName != name) { auto it2 = nameCache.find(resolvedName); if (it2 != nameCache.end() && it2->second.isValid) { it2->second.level = level; it2->second.clanTag = clan; } }
	}
}

void TabList::FetchPlayerStats(const std::string& name) {
	if (name.empty() || name.length() > 16) return;
	int code = 0;
	std::string profileUrl = "https://stats.pika-network.net/api/profile/" + name;
	std::string profileJson = WinHttpGet(profileUrl, code);
	TabListStatsData s; s.isValid = true; s.fetchTime = std::chrono::system_clock::now();
	if (code == 404 || profileJson.find("\"error\"") != std::string::npos) { s.isNicked = true; s.fetchCompleted = true; std::lock_guard<std::mutex> l(cacheMutex); nameCache[name] = s; return; }
	if (code == 429) { { std::lock_guard<std::mutex> l(queueMutex); fetchQueue.push(name); } Sleep(1500); return; }
	if (code != 200 || profileJson.empty()) { s.isStatsOff = true; s.fetchCompleted = true; std::lock_guard<std::mutex> l(cacheMutex); nameCache[name] = s; return; }
	std::string resolvedName = name;
	try {
		json p = json::parse(profileJson);
		if (p.contains("username") && p["username"].is_string()) resolvedName = p["username"].get<std::string>();
		if (p.contains("rank") && p["rank"].is_object() && p["rank"].contains("level")) s.level = p["rank"]["level"].get<int>();
		if (p.contains("clan") && p["clan"].is_object() && p["clan"].contains("tag")) s.clanTag = p["clan"]["tag"].get<std::string>();
	} catch (...) {}
	int lbCode = 0;
	std::string lbUrl = "https://stats.pika-network.net/api/profile/" + resolvedName + "/leaderboard?type=bedwars&interval=lifetime&mode=ALL_MODES";
	std::string lbJson = WinHttpGet(lbUrl, lbCode);
	if (lbCode == 429) { { std::lock_guard<std::mutex> l(queueMutex); fetchQueue.push(name); } Sleep(1500); return; }
	if (lbCode == 200 && !lbJson.empty()) {
		try {
			json lb = json::parse(lbJson);
			if (!lb.empty() && !lb.contains("error") && (lb.contains("Final kills") || lb.contains("Kills") || lb.contains("Wins") || lb.contains("Beds destroyed") || lb.contains("Games played"))) {
				s.hasValidStats = true; s.fetchCompleted = true;
				s.finalKills = ParseStatFromLB(lb, "Final kills"); s.finalDeaths = ParseStatFromLB(lb, "Final deaths");
				s.kills = ParseStatFromLB(lb, "Kills"); s.deaths = ParseStatFromLB(lb, "Deaths");
				s.wins = ParseStatFromLB(lb, "Wins"); s.losses = ParseStatFromLB(lb, "Losses");
				s.bedsBroken = ParseStatFromLB(lb, "Beds destroyed"); s.gamesPlayed = ParseStatFromLB(lb, "Games played");
				s.winstreak = ParseStatFromLB(lb, "Highest winstreak reached");
				s.arrowsShot = ParseStatFromLB(lb, "Arrows shot"); s.arrowsHit = ParseStatFromLB(lb, "Arrows hit");
				s.bowKills = ParseStatFromLB(lb, "Bow kills"); s.voidKills = ParseStatFromLB(lb, "Void kills"); s.meleeKills = ParseStatFromLB(lb, "Melee kills");
				s.missedShots = s.arrowsShot - s.arrowsHit; if (s.missedShots < 0) s.missedShots = 0;
				s.fkdr = s.finalDeaths > 0 ? (float)s.finalKills / s.finalDeaths : (float)s.finalKills;
				s.kdr = s.deaths > 0 ? (float)s.kills / s.deaths : (float)s.kills;
				s.wlr = s.losses > 0 ? (float)s.wins / s.losses : (float)s.wins;
			} else { s.isStatsOff = true; s.fetchCompleted = true; }
		} catch (...) { s.isStatsOff = true; s.fetchCompleted = true; }
	} else { s.isStatsOff = true; s.fetchCompleted = true; }
	{ std::lock_guard<std::mutex> l(cacheMutex); nameCache[name] = s; nameCache[resolvedName] = s; }
}
// calls menu.cpp so it knows what to draw
void TabList::RenderHud() {
	if (!settings::TL_Enabled) return;
	if (!Java::env) return;

	if (!(GetAsyncKeyState(VK_TAB) & 0x8000)) return;
	if (Menu::open) return;

	if (!StrayCache::minecraft_class || !StrayCache::minecraft_getMinecraft || !StrayCache::minecraft_getNetHandler) return;
	if (!StrayCache::netHandlerPlayClient_getPlayerInfoMap && !StrayCache::netHandlerPlayClient_playerInfoList) return;
	if (!StrayCache::networkPlayerInfo_getGameProfile || !StrayCache::gameProfile_class || !StrayCache::gameProfile_getName) return;
	if (!StrayCache::networkPlayerInfo_getResponseTime) return;
	if (!StrayCache::map_values || !StrayCache::collection_iterator || !StrayCache::iterator_hasNext || !StrayCache::iterator_next) return;

	jobject mc = Java::env->CallStaticObjectMethod(StrayCache::minecraft_class, StrayCache::minecraft_getMinecraft);
	if (clearJNI() || !mc) return;
	jobject nh = Java::env->CallObjectMethod(mc, StrayCache::minecraft_getNetHandler);
	if (clearJNI() || !nh) { Java::env->DeleteLocalRef(mc); return; }

	jobject col = nullptr;
	if (StrayCache::netHandlerPlayClient_playerInfoList && StrayCache::map_values) {
		jobject map = Java::env->GetObjectField(nh, StrayCache::netHandlerPlayClient_playerInfoList);
		if (!clearJNI() && map) { col = Java::env->CallObjectMethod(map, StrayCache::map_values); Java::env->DeleteLocalRef(map); }
	}
	if (!col && StrayCache::netHandlerPlayClient_getPlayerInfoMap) {
		col = Java::env->CallObjectMethod(nh, StrayCache::netHandlerPlayClient_getPlayerInfoMap); clearJNI();
	}
	clearJNI();
	Java::env->DeleteLocalRef(nh);

	if (!col) { Java::env->DeleteLocalRef(mc); return; }

	jobject it = Java::env->CallObjectMethod(col, StrayCache::collection_iterator);
	if (clearJNI() || !it) { Java::env->DeleteLocalRef(col); Java::env->DeleteLocalRef(mc); return; }

	struct PlayerRow { std::string name; int ping; std::string statsText; };
	std::vector<PlayerRow> rows;

	const std::string MARKER = " \xC2\xA7r\xC2\xA7r ";
	int limit = 0;
	while (limit++ < 1000) {
		if (!Java::env->CallBooleanMethod(it, StrayCache::iterator_hasNext)) break;
		if (clearJNI()) break;
		jobject pi = Java::env->CallObjectMethod(it, StrayCache::iterator_next);
		if (clearJNI()) break;
		if (!pi) continue;

		std::string rawName;
		jobject gp = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getGameProfile);
		if (!clearJNI() && gp) {
			jstring nj = (jstring)Java::env->CallObjectMethod(gp, StrayCache::gameProfile_getName);
			if (!clearJNI() && nj) {
				const char* c = Java::env->GetStringUTFChars(nj, nullptr);
				if (c) { rawName = c; Java::env->ReleaseStringUTFChars(nj, c); }
				Java::env->DeleteLocalRef(nj);
			}
			Java::env->DeleteLocalRef(gp);
		}
		if (!isValidName(rawName)) { Java::env->DeleteLocalRef(pi); continue; }

		int ping = 0;
		jint pingVal = Java::env->CallIntMethod(pi, StrayCache::networkPlayerInfo_getResponseTime);
		if (!clearJNI()) ping = (int)pingVal;

		TabListStatsData st; bool hs = false;
		{ std::lock_guard<std::mutex> l(cacheMutex); auto f = nameCache.find(rawName); if (f != nameCache.end()) { st = f->second; hs = true; } }
		std::string statStr;
		if (hs) statStr = BuildStatSuffix(st);

		std::string displayName = rawName;
		if (StrayCache::networkPlayerInfo_getDisplayName && StrayCache::ichatcomponent_class && StrayCache::ichatcomponent_getFormattedText) {
			jobject dc = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getDisplayName);
			if (!clearJNI() && dc != nullptr) {
				jstring df = (jstring)Java::env->CallObjectMethod(dc, StrayCache::ichatcomponent_getFormattedText);
				if (!clearJNI() && df != nullptr) {
					const char* dcc = Java::env->GetStringUTFChars(df, nullptr);
					if (dcc) { std::string ed = dcc; Java::env->ReleaseStringUTFChars(df, dcc);
						if (!ed.empty() && ed.find(MARKER) == std::string::npos) displayName = ed;
						else if (ed.find(MARKER) != std::string::npos) displayName = ed.substr(0, ed.find(MARKER));
					}
					Java::env->DeleteLocalRef(df);
				}
				Java::env->DeleteLocalRef(dc);
			}
			clearJNI();
		} else if (StrayCache::networkPlayerInfo_getPlayerTeam && StrayCache::scorePlayerTeam_getColorPrefix) {
			jobject teamObj = Java::env->CallObjectMethod(pi, StrayCache::networkPlayerInfo_getPlayerTeam);
			if (!clearJNI() && teamObj != nullptr) {
				jstring pStr = (jstring)Java::env->CallObjectMethod(teamObj, StrayCache::scorePlayerTeam_getColorPrefix);
				if (!clearJNI() && pStr) {
					const char* pChars = Java::env->GetStringUTFChars(pStr, nullptr);
					if (pChars) { displayName = std::string(pChars) + rawName; Java::env->ReleaseStringUTFChars(pStr, pChars); }
					Java::env->DeleteLocalRef(pStr);
				}
				Java::env->DeleteLocalRef(teamObj);
			}
			clearJNI();
		}

		rows.push_back({ displayName, ping, statStr });
		Java::env->DeleteLocalRef(pi);
	}
	Java::env->DeleteLocalRef(it);
	Java::env->DeleteLocalRef(col);
	Java::env->DeleteLocalRef(mc);
	clearJNI();

	if (rows.empty()) return;

	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.15f, 20), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.7f, ImGui::GetIO().DisplaySize.y - 40), ImGuiCond_Always);

	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

	char overlayTitle[64]; snprintf(overlayTitle, sizeof(overlayTitle), "TabList Stats##TabListOverlay");
	ImGui::Begin(overlayTitle, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

	ImGui::Columns(3, "tablist_cols", false);
	ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);
	ImGui::SetColumnWidth(1, ImGui::GetWindowWidth() * 0.25f);
	ImGui::SetColumnWidth(2, ImGui::GetWindowWidth() * 0.25f);

	ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Player");
	ImGui::NextColumn();
	ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Ping");
	ImGui::NextColumn();
	ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Stats");
	ImGui::NextColumn();
	ImGui::Separator();

	for (auto& row : rows) {
		ImGui::Text("%s", row.name.c_str());
		ImGui::NextColumn();

		int pingVal = row.ping;
		float pingFrac = pingVal > 200 ? 1.0f : pingVal / 200.0f;
		ImVec4 pingColor(0.0f + pingFrac, 1.0f - pingFrac, 0.0f, 1.0f);

		char pingText[16]; snprintf(pingText, sizeof(pingText), "%dms", row.ping);
		ImGui::TextColored(pingColor, "%s", pingText);
		ImGui::NextColumn();

		if (!row.statsText.empty()) {
			std::string clean = row.statsText;
			for (size_t i = 0; i < clean.size(); ) {
				if ((unsigned char)clean[i] == 0xC2 && i + 1 < clean.size() && (unsigned char)clean[i+1] == 0xA7) { clean.erase(i, 2); continue; }
				i++;
			}
			while (!clean.empty() && clean[0] == ' ') clean.erase(0, 1);
			ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", clean.c_str());
		}
		ImGui::NextColumn();
	}

	ImGui::Columns(1);
	ImGui::End();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(2);
}

void TabList::RenderMenu() {
	Menu::Checkbox("Enable TabList Stats", &settings::TL_Enabled);
	if (!settings::TL_Enabled) return;
	ImGui::Separator(); ImGui::Text("Appearance");
	ImGui::Combo("Format Mode", &settings::TL_FormatMode, settings::TL_FormatModeList, 2);
	Menu::Checkbox("Hide in Lobby", &settings::TL_HideInLobby);
	Menu::Checkbox("Use Threshold Colors", &settings::TL_UseThresholdColors);
	if (settings::TL_FormatMode == 1) { ImGui::InputText("Custom String", settings::TL_FormatString, sizeof(settings::TL_FormatString)); }
	ImGui::Separator(); ImGui::Text("API Settings");
	ImGui::Combo("Interval", &settings::TL_Interval, settings::TL_IntervalList, 4);
	ImGui::Combo("Mode", &settings::TL_Mode, settings::TL_ModeList, 4);
	ImGui::Separator(); ImGui::Columns(2, "tl", false);
	Menu::Checkbox("Show Level", &settings::TL_showLevel); Menu::Checkbox("Show FKDR", &settings::TL_showFkdr);
	Menu::Checkbox("Show KDR", &settings::TL_showKdr); Menu::Checkbox("Show WLR", &settings::TL_showWlr);
	Menu::Checkbox("Show Winstreak", &settings::TL_showWS); Menu::Checkbox("Show Wins", &settings::TL_showWins);
	Menu::Checkbox("Show Losses", &settings::TL_showLosses); Menu::Checkbox("Show Games", &settings::TL_showGames);
	Menu::Checkbox("Show Beds", &settings::TL_showBeds);
	ImGui::NextColumn();
	Menu::Checkbox("Show Final Kills", &settings::TL_showFinalKills); Menu::Checkbox("Show Final Deaths", &settings::TL_showFinalDeaths);
	Menu::Checkbox("Show Kills", &settings::TL_showKills); Menu::Checkbox("Show Deaths", &settings::TL_showDeaths);
	Menu::Checkbox("Show Arrows", &settings::TL_showArrows); Menu::Checkbox("Show Arrows Hit", &settings::TL_showArrowsHit);
	Menu::Checkbox("Show Missed", &settings::TL_showMissedShots); Menu::Checkbox("Show Bow Kills", &settings::TL_showBowKills);
	Menu::Checkbox("Show Void Kills", &settings::TL_showVoidKills); Menu::Checkbox("Show Melee Kills", &settings::TL_showMeleeKills);
	ImGui::Columns(1); ImGui::Separator();
	if (ImGui::TreeNode("Color & Threshold Customization")) {
		ImGui::Text("Base Colors");
		ImGui::ColorEdit3("Level Color", settings::TL_Col_Level, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
		ImGui::ColorEdit3("Wins Color", settings::TL_Col_Wins, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
		ImGui::ColorEdit3("Losses Color", settings::TL_Col_Losses, ImGuiColorEditFlags_NoInputs);
		ImGui::ColorEdit3("Beds Color", settings::TL_Col_Beds, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
		ImGui::ColorEdit3("Games Color", settings::TL_Col_Games, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
		ImGui::ColorEdit3("Arrows Color", settings::TL_Col_Arrows, ImGuiColorEditFlags_NoInputs);
		ImGui::ColorEdit3("N/A Color", settings::TL_Col_NA, ImGuiColorEditFlags_NoInputs); ImGui::SameLine();
		ImGui::ColorEdit3("Nicked Color", settings::TL_Col_Nicked, ImGuiColorEditFlags_NoInputs);
		ImGui::Separator(); ImGui::Text("FKDR/KDR/WLR Color Thresholds");
		ImGui::Text("Players with ratio ABOVE these values get that color:");
		ImGui::InputFloat("Ratio >= ##t1", &settings::TL_Thresh_Ratio_High, 0.5f, 1.0f, "%.1f"); ImGui::SameLine(); ImGui::ColorEdit3("##cr1", settings::TL_Col_Ratio_High, ImGuiColorEditFlags_NoInputs);
		ImGui::InputFloat("Ratio >= ##t2", &settings::TL_Thresh_Ratio_Med, 0.5f, 1.0f, "%.1f"); ImGui::SameLine(); ImGui::ColorEdit3("##cr2", settings::TL_Col_Ratio_Med, ImGuiColorEditFlags_NoInputs);
		ImGui::InputFloat("Ratio >= ##t3", &settings::TL_Thresh_Ratio_Low, 0.1f, 0.5f, "%.1f"); ImGui::SameLine(); ImGui::ColorEdit3("##cr3", settings::TL_Col_Ratio_Low, ImGuiColorEditFlags_NoInputs);
		ImGui::Text("Below lowest threshold:"); ImGui::SameLine(); ImGui::ColorEdit3("##crdef", settings::TL_Col_Ratio_Def, ImGuiColorEditFlags_NoInputs);
		ImGui::Separator(); ImGui::Text("Winstreak Color Thresholds");
		ImGui::Text("Players with WS ABOVE these values get that color:");
		ImGui::InputFloat("WS >= ##ws1", &settings::TL_Thresh_WS_High, 5.0f, 10.0f, "%.0f"); ImGui::SameLine(); ImGui::ColorEdit3("##cw1", settings::TL_Col_WS_High, ImGuiColorEditFlags_NoInputs);
		ImGui::InputFloat("WS >= ##ws2", &settings::TL_Thresh_WS_Med, 5.0f, 10.0f, "%.0f"); ImGui::SameLine(); ImGui::ColorEdit3("##cw2", settings::TL_Col_WS_Med, ImGuiColorEditFlags_NoInputs);
		ImGui::InputFloat("WS >= ##ws3", &settings::TL_Thresh_WS_Low, 1.0f, 5.0f, "%.0f"); ImGui::SameLine(); ImGui::ColorEdit3("##cw3", settings::TL_Col_WS_Low, ImGuiColorEditFlags_NoInputs);
		ImGui::Text("Below lowest threshold:"); ImGui::SameLine(); ImGui::ColorEdit3("##cwdef", settings::TL_Col_WS_Def, ImGuiColorEditFlags_NoInputs);
		ImGui::TreePop();
	}
	ImGui::Separator();
	ImGui::Text("Target Warning (comma separated)");
	ImGui::InputText("##TW", settings::TL_TargetWarningNames, sizeof(settings::TL_TargetWarningNames));
	ImGui::Separator();
	// dosent work (duo to incorrect mappings, need to be fixed)
	ImGui::Text("Anti-Spam Settings");
	Menu::Checkbox("Block Sale/Title Spam", &settings::AntiSpam_Enabled);
	static char customKeywordBuf[64] = "";
	ImGui::InputText("##keyword_input", customKeywordBuf, sizeof(customKeywordBuf));
	ImGui::SameLine();
	if (ImGui::Button("Add Keyword") && customKeywordBuf[0] != '\0') {
		settings::AntiSpam_Keywords.push_back(std::string(customKeywordBuf));
		memset(customKeywordBuf, 0, sizeof(customKeywordBuf));
	}
	ImGui::BeginChild("##keywords_list", ImVec2(0, 100), true);
	for (int i = 0; i < (int)settings::AntiSpam_Keywords.size(); i++) {
		ImGui::Text("%s", settings::AntiSpam_Keywords[i].c_str());
		ImGui::SameLine();
		char lb[32]; snprintf(lb, 32, "X##kw%d", i);
		if (ImGui::Button(lb)) {
			settings::AntiSpam_Keywords.erase(settings::AntiSpam_Keywords.begin() + i);
			i--;
		}
	} // the cache works (lets you clear cache to remove the remembered stats to not spam the API too much and get rate limited
	ImGui::EndChild();
	ImGui::Separator(); if (ImGui::Button("Clear Cache")) ClearCache();
}