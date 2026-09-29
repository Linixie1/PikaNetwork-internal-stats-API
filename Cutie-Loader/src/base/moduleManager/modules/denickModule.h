#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

class DenickModule {
public:
    DenickModule() = delete;
    static void RenderOverlay();
    // call once at startup
    static void Initialize();
    static void Shutdown();

    // hook this up to the license box in the gui
    static void SetLicenseKey(std::string key);
    static std::string GetLicenseKey();

    // returns "" if we cant figure it out
    static std::string ResolveRealName(const std::string& fakeName);

    static std::string StaffApiGet(const std::string& path);
private:
    struct Session {
        std::string token;
        std::string userAgent;
        std::string domain;
        std::string cookies;
        int64_t     expiresAt = 0;
    };

    struct CacheEntry {
        std::string value;
        int64_t     expiresAt = 0;
        std::list<std::string>::iterator lruIt;
    };

    static void        RefreshLoop();
    static bool        FetchSession(Session& out);
    static std::string HttpGet(const std::string& url,
                               const std::string& headers,
                               int* statusOut);

    static Session     SnapshotSession();
    static std::string CallDenickApi(const std::string& nick, const Session& s);

    static std::string UrlEncode(const std::string& in);

    // tiny json helpers, no deps
    static bool JsonFindValue(const std::string& body, const std::string& key, size_t& outPos);
    static bool JsonParseString(const std::string& body, size_t pos, std::string& out);
    static bool JsonParseInt64(const std::string& body, size_t pos, int64_t& out);

    static int64_t NowSeconds();

    static std::mutex sessionMutex;
    static Session    session;

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, CacheEntry> cache;
    static std::list<std::string> lruOrder;

    static std::mutex  licenseMutex;
    static std::string licenseKey;

    static std::atomic<bool> running;
    static std::atomic<bool> started;
};