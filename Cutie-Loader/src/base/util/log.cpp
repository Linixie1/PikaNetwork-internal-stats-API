#include "log.h"
#include <Windows.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

static std::string GetExeDir()
{
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (len == 0) return ".";
    std::string path(buffer, len);
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        path = path.substr(0, pos);
    return path;
}

Logger& Logger::GetInstance()
{
    static Logger instance;
    return instance;
}

Logger::Logger()
{
    std::string logPath = GetExeDir() + "\\cutie-loader.log";
    m_file.open(logPath, std::ios::out | std::ios::trunc);
    if (m_file.is_open())
    {
        m_file << "cutie loader log \n";
        m_file.flush();
    }
}

Logger::~Logger()
{
    if (m_file.is_open())
        m_file.close();
}

void Logger::Write(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm localTime;
    localtime_s(&localTime, &time);

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &localTime);

    if (m_file.is_open())
    {
        m_file << "[" << timeBuf << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
               << message << "\n";
        m_file.flush();
    }

    // writes every debug log to dbgview
    OutputDebugStringA(("[cutie] " + message + "\n").c_str());
}