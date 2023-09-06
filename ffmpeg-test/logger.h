#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR
};
#define LOG(level, ...) Logger::getInstance().enqueueLog(LogLevel::level, __FUNCTION__, __LINE__,__VA_ARGS__)
class Logger
{
public:
    static Logger &getInstance()
    {
        static Logger instance;
        return instance;
    }

//    void setFilename(const std::string &filename)
//    {
//        m_filename = filename;
//    }
    template <typename... Args>
    void enqueueLog(LogLevel level, const std::string functionName, int lineNumber,Args... args)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::stringstream ss;
        // 将额外参数添加到字符串流
        ((ss << args << " "), ...);
        m_logQueue.push(LogEntry{level, ss.str(), functionName, lineNumber});
        m_condition.notify_one();
    }
    // Logger类不可拷贝和移动
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

private:
    struct LogEntry
    {
        LogLevel level;
        std::string message;
        std::string functionName;
        int lineNumber;
    };

private:

    std::string logLevelToString(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO ";
        case LogLevel::WARN:
            return "WARN ";
        case LogLevel::ERROR:
            return "ERROR";
        default:
            return "UNKNOW";
        }
    }

    Logger() : m_isRunning(true)
    {
        m_writerThread = std::thread(&Logger::writeLogs, this);
    }

    ~Logger()
    {
        m_isRunning = false;
        m_condition.notify_one();
        m_writerThread.join();
    }

    void writeLogs()
    {
//        std::ofstream file(m_filename, std::ios::app);
//        if (!file.is_open())
//        {
//            std::cerr  << formatTimeStamp()<< "Error opening log file!" << std::endl;
//            return;
//        }

        while (m_isRunning || !m_logQueue.empty())
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this]
                             { return !m_logQueue.empty() || !m_isRunning; });

            while (!m_logQueue.empty())
            {
                LogEntry logEntry = m_logQueue.front();
                m_logQueue.pop();

                lock.unlock();

                std::string levelStr;
                switch (logEntry.level)
                {
                case LogLevel::DEBUG:
                    levelStr = "DEBUG";
                    break;
                case LogLevel::INFO:
                    levelStr = "INFO ";
                    break;
                case LogLevel::WARN:
                    levelStr = "WARN ";
                    break;
                case LogLevel::ERROR:
                    levelStr = "ERROR";
                    break;
                default:
                    levelStr = "UNKNOW";
                    return;
                }

//                file << "[" << formatTimeStamp() << "] "
//                     << "[" << logEntry.functionName << ":" << logEntry.lineNumber << "] "
//                     << "[" << levelStr << "], " << logEntry.message << std::endl;

                std::cout << "[" << formatTimeStamp() << "] "
                          << "[" << logEntry.functionName << ":" << logEntry.lineNumber << "] "
                          << "[" << levelStr << "]: " << logEntry.message << std::endl;

                lock.lock();
            }

            //file.flush();
        }

        //file.close();
    }

    std::string formatTimeStamp()
    {
        // ...（与之前代码中formatTimeStamp方法的实现相同）
        // get time point
        auto currentTime = std::chrono::system_clock::now();
        // 将时间戳转换为毫秒级别的时间
        auto timestamp = std::chrono::time_point_cast<std::chrono::milliseconds>(currentTime).time_since_epoch().count();
        // 将时间值转换为本地时间的 tm 结构。
        std::time_t time = std::chrono::system_clock::
            to_time_t(std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp)));
        std::tm *tm = std::localtime(&time);
        // 类创建一个输出字符串流，
        // 使用 std::put_time() 和格式化字符串 "%Y-%m-%d %H:%M:%S"
        // 将 tm 结构格式化为日期和时间部分。通过将毫秒部分转换为字符串并
        // 使用 std::setfill() 和 std::setw() 设置输出宽度，可以在字符串流中添加毫秒级精度。
        // 通过使用 std::setfill('0') 设置填充字符为零，并使用 std::setw(3) 设置输出宽度为 3 位，
        // 将毫秒部分添加到字符串流中。最后，将字符串流转换为 std::string 类型，并使用 oss.str() 返回格式化的时间戳字符串。
        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S.") << std::setfill('0') << std::setw(3) << timestamp % 1000;
        return oss.str();
    }

    std::queue<LogEntry> m_logQueue;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_isRunning;
    std::string m_filename;
    std::thread m_writerThread;
};


#endif // LOGGER_H
