#include "denickModule.h"
#include "tabListModule.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinInet.h>

#pragma comment(lib, "wininet.lib")

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <vector>

namespace {

constexpr int64_t kCacheTtlSeconds       = 600;
constexpr int64_t kNegativeCacheTtl      = 30;
constexpr size_t  kCacheMaxEntries       = 2048;
constexpr int64_t kRefreshSafetyMargin   = 10;
constexpr int64_t kMinRefreshSeconds     = 5;
constexpr int64_t kBackoffBaseSeconds    = 5;
constexpr int64_t kBackoffCeilingSeconds = 60;

// keep the host out of the binary's string table, trivial deobf but enough
constexpr unsigned char kObfSeed = 0x9D;

template <size_t N>
struct Obf {
    char data[N] = {};
    constexpr Obf(const char (&s)[N]) {
        for (size_t i = 0; i < N; ++i)
            data[i] = static_cast<char>(s[i] ^ (kObfSeed + (i & 0x1F)));
    }
    std::string reveal() const {
        std::string out;
        out.reserve(N - 1);
        for (size_t i = 0; i + 1 < N; ++i)
            out += static_cast<char>(data[i] ^ (kObfSeed + (i & 0x1F)));
        return out;
    }
};

constexpr Obf kAuthEndpoint{ "https://..//." };

size_t SkipWs(const std::string& s, size_t i) {
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
        else break;
    }
    return i;
}

} // namespace

std::mutex            DenickModule::sessionMutex;
DenickModule::Session DenickModule::session;

std::mutex            DenickModule::cacheMutex;
std::unordered_map<std::string, DenickModule::CacheEntry> DenickModule::cache;
std::list<std::string> DenickModule::lruOrder;

std::mutex            DenickModule::licenseMutex;
std::string           DenickModule::licenseKey;

std::atomic<bool>     DenickModule::running{false};
std::atomic<bool>     DenickModule::started{false};

std::string DenickModule::StaffApiGet(const std::string& path) {
    const Session s = SnapshotSession();
    if (s.token.empty() || s.domain.empty()) return {};

    const std::string url = "https://" + s.domain + path;

    std::string headers;
    headers += "Authorization: Bearer " + s.token + "\r\n";
    headers += "Accept: application/json\r\n";
    if (!s.cookies.empty())
        headers += "Cookie: " + s.cookies + "\r\n";

    int status = 0;
    const std::string body = HttpGet(url, headers, &status);
    if (status < 200 || status >= 300) return {};
    return body;
}

void DenickModule::Initialize() {
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    running.store(true);
    std::thread(&DenickModule::RefreshLoop).detach();
}

void DenickModule::Shutdown() {
    running.store(false);
    started.store(false);
}

void DenickModule::SetLicenseKey(std::string key) {
    {
        std::lock_guard<std::mutex> lk(licenseMutex);
        licenseKey = std::move(key);
    }

    // wipe the session so a token minted under the old key can't linger
    std::lock_guard<std::mutex> lk(sessionMutex);
    session = Session{};
}

std::string DenickModule::GetLicenseKey() {
    std::lock_guard<std::mutex> lk(licenseMutex);
    return licenseKey;
}


int64_t DenickModule::NowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string DenickModule::HttpGet(const std::string& url,
                                  const std::string& headers,
                                  int* statusOut) {
    if (statusOut) *statusOut = 0;

    std::string ua = "Mozilla/5.0";
    {
        std::lock_guard<std::mutex> lk(sessionMutex);
        if (!session.userAgent.empty()) ua = session.userAgent;
    }

    HINTERNET hNet = InternetOpenA(ua.c_str(),
                                   INTERNET_OPEN_TYPE_PRECONFIG,
                                   nullptr, nullptr, 0);
    if (!hNet) return {};

    // wininet drops any header line that isn't crlf terminated
    std::string hdr = headers;
    if (!hdr.empty() &&
        hdr.compare(hdr.size() >= 2 ? hdr.size() - 2 : 0, 2, "\r\n") != 0) {
        hdr += "\r\n";
    }

    // secure flag makes wininet reject anything that isn't a valid https chain
    const DWORD flags = INTERNET_FLAG_RELOAD
                      | INTERNET_FLAG_NO_COOKIES
                      | INTERNET_FLAG_SECURE
                      | INTERNET_FLAG_NO_UI;

    HINTERNET hReq = InternetOpenUrlA(hNet,
                                      url.c_str(),
                                      hdr.empty() ? nullptr : hdr.c_str(),
                                      static_cast<DWORD>(hdr.size()),
                                      flags, 0);
    if (!hReq) {
        InternetCloseHandle(hNet);
        return {};
    }

    if (statusOut) {
        DWORD code = 0;
        DWORD len  = sizeof(code);
        DWORD idx  = 0;
        if (HttpQueryInfoA(hReq,
                           HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &code, &len, &idx)) {
            *statusOut = static_cast<int>(code);
        }
    }
    std::string body;
    char  buf[8192];

    DWORD got = 0;
    while (InternetReadFile(hReq, buf, sizeof(buf), &got) && got > 0)
        body.append(buf, got);

    InternetCloseHandle(hReq);
    InternetCloseHandle(hNet);

    return body;
}

bool DenickModule::FetchSession(Session& out) {
    std::string key;
    {
        std::lock_guard<std::mutex> lk(licenseMutex);
        key = licenseKey;
    }
    if (key.empty()) return false;

    const std::string url = kAuthEndpoint.reveal() + "?license=" + UrlEncode(key);

    int status = 0;
    const std::string body = HttpGet(url, {}, &status);
    if (status < 200 || status >= 300 || body.empty())
        return false;

    size_t pos = 0;

    if (!JsonFindValue(body, "token", pos))       return false;
    if (!JsonParseString(body, pos, out.token))   return false;

    if (!JsonFindValue(body, "api_domain", pos))  return false;
    if (!JsonParseString(body, pos, out.domain))  return false;

    if (!JsonFindValue(body, "user_agent", pos))  return false;
    if (!JsonParseString(body, pos, out.userAgent)) return false;

    if (!JsonFindValue(body, "cookie_data", pos)) return false;
    if (!JsonParseString(body, pos, out.cookies)) return false;

    int64_t ttl = 0;

    if (!JsonFindValue(body, "expires_in", pos))  return false;
    if (!JsonParseInt64(body, pos, ttl))          return false;

    if (out.token.empty() || out.domain.empty() || ttl <= 0)
        return false;

    out.expiresAt = NowSeconds() + ttl;
    return true;
}

void DenickModule::RefreshLoop() {
    int64_t backoff = kBackoffBaseSeconds;

    while (running.load()) {
        Session fresh;
        if (FetchSession(fresh)) {
            int64_t ttl = 0;
            {
                std::lock_guard<std::mutex> lk(sessionMutex);
                session = std::move(fresh);
                ttl = session.expiresAt - NowSeconds();
            }
            backoff = kBackoffBaseSeconds;

            // aim for the midpoint so a slow round trip still lands in time
            const int64_t nap = std::max<int64_t>(
                kMinRefreshSeconds, (ttl - kRefreshSafetyMargin) / 2);

            for (int64_t slept = 0; slept < nap && running.load(); slept += 250)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
        } else {
            {
                std::lock_guard<std::mutex> lk(sessionMutex);
                session.token.clear();
                session.expiresAt = 0;
            }

            for (int64_t slept = 0; slept < backoff && running.load(); slept += 250)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));

            backoff = std::min<int64_t>(backoff * 2, kBackoffCeilingSeconds);
        }
    }
}

DenickModule::Session DenickModule::SnapshotSession() {
    std::lock_guard<std::mutex> lk(sessionMutex);
    if (session.token.empty() || NowSeconds() >= session.expiresAt)
        return Session{};
    return session;
}

std::string DenickModule::CallDenickApi(const std::string& nick,
                                        const Session& s) {
    if (s.domain.empty() || s.token.empty()) return {};

    const std::string url = "https://" + s.domain
                          + "///denick?=" + UrlEncode(nick);

    std::string headers;
    headers += "Authorization: Bearer " + s.token + "\r\n";
    headers += "Accept: application/json\r\n";
    if (!s.cookies.empty())
        headers += "Cookie: " + s.cookies + "\r\n";

    int status = 0;
    const std::string body = HttpGet(url, headers, &status);
    if (status < 200 || status >= 300) return {};

    size_t pos = 0;
    if (!JsonFindValue(body, "real_name", pos)) return {};

    std::string real;
    if (!JsonParseString(body, pos, real)) return {};
    return real;
}

std::string DenickModule::ResolveRealName(const std::string& fakeName) {
    if (fakeName.empty()) return {};

    const int64_t now = NowSeconds();

    {
        std::lock_guard<std::mutex> lk(cacheMutex);
        auto it = cache.find(fakeName);
        if (it != cache.end()) {
            if (now < it->second.expiresAt) {
                // move to the tail so it's the last thing evicted
                lruOrder.erase(it->second.lruIt);
                lruOrder.push_back(fakeName);
                it->second.lruIt = std::prev(lruOrder.end());
                return it->second.value;
            }
            lruOrder.erase(it->second.lruIt);
            cache.erase(it);
        }
    }

    // snapshot and release before the round trip, otherwise every other
    // caller queues behind this mutex for the duration of the request
    const Session s = SnapshotSession();
    if (s.token.empty()) return {};

    const std::string real = CallDenickApi(fakeName, s);

    {
        std::lock_guard<std::mutex> lk(cacheMutex);

        const int64_t ttl = real.empty() ? kNegativeCacheTtl : kCacheTtlSeconds;

        auto it = cache.find(fakeName);
        if (it != cache.end()) {
            lruOrder.erase(it->second.lruIt);
            cache.erase(it);
        }

        CacheEntry e;
        e.value     = real;
        e.expiresAt = now + ttl;
        lruOrder.push_back(fakeName);
        e.lruIt = std::prev(lruOrder.end());
        cache.emplace(fakeName, std::move(e));

        while (cache.size() > kCacheMaxEntries && !lruOrder.empty()) {
            const std::string victim = lruOrder.front();
            lruOrder.pop_front();
            cache.erase(victim);
        }
    }

    if (!real.empty())
        TabList::InjectRealName(fakeName, real);

    return real;
}

std::string DenickModule::UrlEncode(const std::string& in) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);

    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

bool DenickModule::JsonFindValue(const std::string& body,
                                 const std::string& key,
                                 size_t& outPos) {
    const std::string needle = "\"" + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return false;

    p = SkipWs(body, p + needle.size());
    if (p >= body.size() || body[p] != ':') return false;
    p = SkipWs(body, p + 1);
    if (p >= body.size()) return false;

    outPos = p;
    return true;
}

bool DenickModule::JsonParseString(const std::string& body,
                                   size_t pos,
                                   std::string& out) {
    if (pos >= body.size() || body[pos] != '"') return false;
    ++pos;
    out.clear();

    while (pos < body.size()) {
        const char c = body[pos++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (pos >= body.size()) return false;

        const char e = body[pos++];
        switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                // no surrogate pair handling, the api doesn't emit them
                if (pos + 4 > body.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = body[pos + k];
                    cp <<= 4;
                    if      (h >= '0' && h <= '9') cp |= (h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                    else return false;
                }
                pos += 4;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool DenickModule::JsonParseInt64(const std::string& body,
                                  size_t pos,
                                  int64_t& out) {
    const size_t start = pos;
    if (pos < body.size() && (body[pos] == '-' || body[pos] == '+')) ++pos;
    while (pos < body.size() &&
           std::isdigit(static_cast<unsigned char>(body[pos]))) {
        ++pos;
    }
    if (pos == start) return false;
    try {
        out = std::stoll(body.substr(start, pos - start));
        return true;
    } catch (...) {
        return false;
    }
}
void DenickModule::RenderOverlay() {
    if (!ImGui::GetCurrentContext()) return;

    static bool open = true;
    if (!open) return;

    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Denick##dnk_overlay", &open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    Session s;
    {
        std::lock_guard<std::mutex> lk(sessionMutex);
        s = session;
    }

    const int64_t now = NowSeconds();
    const int64_t left = s.expiresAt > now ? s.expiresAt - now : 0;

    std::string key;
    {
        std::lock_guard<std::mutex> lk(licenseMutex);
        key = licenseKey;
    }

    if (key.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "no license");
    } else if (s.token.empty()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "awaiting session");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "connected");
    }

    ImGui::Separator();
    ImGui::Text("key    %s", key.empty() ? "-" :
        (key.substr(0, std::min<size_t>(6, key.size())) + "...").c_str());
    ImGui::Text("domain %s", s.domain.empty() ? "-" : s.domain.c_str());
    ImGui::Text("token  %s", s.token.empty() ? "-" :
        (s.token.substr(0, std::min<size_t>(10, s.token.size())) + "...").c_str());
    ImGui::Text("ttl    %llds", (long long)left);

    ImGui::Separator();
    size_t cached = 0;
    {
        std::lock_guard<std::mutex> lk(cacheMutex);
        cached = cache.size();
    }
    ImGui::Text("cached %zu nicks", cached);

    if (!key.empty()) {
        if (ImGui::Button("refresh")) {
            std::lock_guard<std::mutex> lk(sessionMutex);
            session = Session{};
        }
    }

    ImGui::End();
}