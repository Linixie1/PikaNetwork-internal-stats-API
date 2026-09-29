#include "specDetector.h"
#include "denickModule.h"
#include "tabListModule.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace {

// pull a "key":"value" out of a flat json object
bool FindString(const std::string& body, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return false;

    p = body.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;

    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
    if (p >= body.size() || body[p] != '"') return false;
    ++p;

    out.clear();
    while (p < body.size()) {
        char c = body[p++];
        if (c == '"') return true;
        if (c == '\\' && p < body.size()) c = body[p++];
        out += c;
    }
    return false;
}

// flat array of objects, no nested braces or braces inside strings assumed
std::vector<std::string> SplitObjects(const std::string& arr) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < arr.size()) {
        const size_t s = arr.find('{', i);
        if (s == std::string::npos) break;
        const size_t e = arr.find('}', s);
        if (e == std::string::npos) break;
        out.push_back(arr.substr(s, e - s + 1));
        i = e + 1;
    }
    return out;
}

} // namespace

std::mutex SpecDetector::specMutex;
std::vector<std::string> SpecDetector::specs;

std::mutex SpecDetector::seenMutex;
std::vector<std::string> SpecDetector::seen;

std::mutex SpecDetector::localMutex;
std::string SpecDetector::localPlayer;

std::atomic<bool> SpecDetector::running{false};
std::atomic<bool> SpecDetector::started{false};

void SpecDetector::Initialize() {
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    running.store(true);
    std::thread(&SpecDetector::PollLoop).detach();
}

void SpecDetector::Shutdown() {
    running.store(false);
    started.store(false);
}

void SpecDetector::SetLocalPlayer(const std::string& name) {
    std::lock_guard<std::mutex> lk(localMutex);
    localPlayer = name;
}

std::vector<std::string> SpecDetector::GetSpectators() {
    std::lock_guard<std::mutex> lk(specMutex);
    return specs;
}

std::string SpecDetector::FetchMapId(const std::string& player) {
    const std::string body =
        DenickModule::StaffApiGet("/internal/match/location?player=" + player);
    if (body.empty()) return {};

    std::string mapId;
    if (!FindString(body, "map_id", mapId)) return {};
    return mapId;
}

std::vector<SpecDetector::Player> SpecDetector::FetchPlayers(const std::string& mapId) {
    std::vector<Player> out;

    const std::string body =
        DenickModule::StaffApiGet("/internal/match/spectators?map=" + mapId);
    if (body.empty()) return out;

    // shape is {"spectators":[{"nick":"..","real_name":".."}, ...]}
    const size_t key = body.find("\"spectators\"");
    if (key == std::string::npos) return out;

    const size_t lb = body.find('[', key);
    const size_t rb = body.find(']', lb);
    if (lb == std::string::npos || rb == std::string::npos) return out;

    for (const auto& obj : SplitObjects(body.substr(lb, rb - lb + 1))) {
        Player p;
        if (!FindString(obj, "nick", p.nick)) continue;
        FindString(obj, "real_name", p.realName);   // stays empty when not nicked
        out.push_back(std::move(p));
    }
    return out;
}

bool SpecDetector::IsKnown(const std::string& nick) {
    std::lock_guard<std::mutex> lk(seenMutex);
    return std::find(seen.begin(), seen.end(), nick) != seen.end();
}

void SpecDetector::HandleNewPlayer(const Player& p) {
    {
        std::lock_guard<std::mutex> lk(seenMutex);
        seen.push_back(p.nick);
    }

    {
        std::lock_guard<std::mutex> lk(specMutex);
        specs.push_back(p.nick);
    }

    // swap in the real name if the api gave us one, tab list handles the colour
    const std::string shown = p.realName.empty() ? p.nick : p.realName;
    TabList::InjectRealName(p.nick, shown);

    // hook this to whatever your chat module exposes for local messages,
    // something like Chat::Print below. section sign is the mc colour prefix
    const std::string line = std::string("\xC2\xA7c[spec] \xC2\xA7f") + p.nick +
        (p.realName.empty() ? "" : std::string(" \xC2\xA77(") + p.realName + ")");
    (void)line;
    // Chat::Print(line);
}

void SpecDetector::PollLoop() {
    using namespace std::chrono;

    while (running.load()) {
        std::string player;
        {
            std::lock_guard<std::mutex> lk(localMutex);
            player = localPlayer;
        }

        if (!player.empty()) {
            const std::string mapId = FetchMapId(player);
            if (!mapId.empty()) {
                for (const auto& p : FetchPlayers(mapId)) {
                    if (!IsKnown(p.nick)) {
                        HandleNewPlayer(p);
                    }
                }
            }
        }

        // 2s tick, chopped up so shutdown doesn't wait on a full sleep
        for (int i = 0; i < 8 && running.load(); ++i)
            std::this_thread::sleep_for(milliseconds(250));
    }
}

void SpecDetector::RenderOverlay() {
    if (!ImGui::GetCurrentContext()) return;

    static bool open = true;
    if (!open) return;

    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Spectators##spec_overlay", &open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> lk(specMutex);
        list = specs;
    }

    std::string me;
    {
        std::lock_guard<std::mutex> lk(localMutex);
        me = localPlayer;
    }

    if (me.empty()) {
        ImGui::TextDisabled("no local player");
    } else {
        ImGui::Text("watching %s", me.c_str());
    }

    ImGui::Separator();

    if (list.empty()) {
        ImGui::TextDisabled("none");
    } else {
        for (const auto& n : list) {
            std::string real;
            bool flagged = false;
            {
                std::lock_guard<std::mutex> lk(DenickModule::cacheMutex);
                auto it = DenickModule::cache.find(n);
                (void)it;
            }
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", n.c_str());
        }
        ImGui::Separator();
        ImGui::Text("%zu total", list.size());
    }

    ImGui::End();
}