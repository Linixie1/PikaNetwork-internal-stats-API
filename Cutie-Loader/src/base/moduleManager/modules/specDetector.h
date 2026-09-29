#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class SpecDetector {
public:
    SpecDetector() = delete;

    static void Initialize();
    static void Shutdown();

    // set this whenever the local player's name changes, the poll loop
    // uses it to look up which map we're in
    static void SetLocalPlayer(const std::string& name);

    static std::vector<std::string> GetSpectators();
    static void RenderOverlay();    
private:
    struct Player {
        std::string nick;
        std::string realName;   // empty when the api doesn't know them
    };

    static void PollLoop();
    static std::string FetchMapId(const std::string& player);
    static std::vector<Player> FetchPlayers(const std::string& mapId);
    static void HandleNewPlayer(const Player& p);
    static bool IsKnown(const std::string& nick);

    static std::mutex specMutex;
    static std::vector<std::string> specs;

    static std::mutex seenMutex;
    static std::vector<std::string> seen;

    static std::mutex localMutex;
    static std::string localPlayer;

    static std::atomic<bool> running;
    static std::atomic<bool> started;
};