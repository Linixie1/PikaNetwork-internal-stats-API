#pragma once

#include <string>
#include <mutex>
#include <fstream>

/// <summary>
/// Thread-safe file logger. Writes to cutie-loader.log in the
/// same directory as the injector executable.
/// </summary>
class Logger
{
public:
    static Logger& GetInstance();

    /// <summary>Append a timestamped line to the log file.</summary>
    void Write(const std::string& message);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex m_mutex;
    std::ofstream m_file;
};