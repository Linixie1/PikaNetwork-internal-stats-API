#pragma once

#include <string>
#include <mutex>
#include <fstream>


class Logger
{
public:
    static Logger& GetInstance();

    void Write(const std::string& message);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex m_mutex;
    std::ofstream m_file;
};