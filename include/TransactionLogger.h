#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

struct LogField {
    std::string key;
    std::string value;
};

class TransactionLogger {
public:
    class ScopedContext {
    public:
        ScopedContext(std::string uuid, std::string channel);
        ~ScopedContext();

        ScopedContext(const ScopedContext&) = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;

    private:
        std::string previousUuid_;
        std::string previousChannel_;
    };

    class ScopedFunctionTrace {
    public:
        explicit ScopedFunctionTrace(std::string functionName,
                                     std::vector<LogField> fields = {},
                                     std::source_location location =
                                         std::source_location::current());
        ~ScopedFunctionTrace();

        ScopedFunctionTrace(const ScopedFunctionTrace&) = delete;
        ScopedFunctionTrace& operator=(const ScopedFunctionTrace&) = delete;

        void checkpoint(std::string_view step,
                        std::string_view message,
                        const std::vector<LogField>& fields = {},
                        std::source_location location =
                            std::source_location::current()) const;
        void fail(std::string_view reason,
                  const std::vector<LogField>& fields = {},
                  std::source_location location =
                      std::source_location::current());
        void success(const std::vector<LogField>& fields = {},
                     std::source_location location =
                         std::source_location::current());

    private:
        std::string functionName_;
        std::string sourceFile_;
        std::uint_least32_t sourceLine_ = 0;
        std::string sourceFunction_;
        std::chrono::steady_clock::time_point startedAt_;
        int exceptionCount_ = 0;
        bool completed_ = false;
    };

    static TransactionLogger& instance();

    void initialize(
        std::filesystem::path logDirectory = std::filesystem::path("bin") / "debug" / "log",
        std::uintmax_t maxFileBytes = 1024ULL * 1024ULL * 1024ULL);
    void shutdown();

    [[nodiscard]] std::string generateUuid() const;
    [[nodiscard]] std::filesystem::path logDirectory() const;

    static std::string currentUuid();
    static std::string currentChannel();

    void log(std::string_view level,
             std::string_view uuid,
             std::string_view channel,
             std::string_view event,
             std::string_view message,
             const std::vector<LogField>& fields = {},
             std::source_location location = std::source_location::current());

    void logCurrent(std::string_view level,
                    std::string_view event,
                    std::string_view message,
                    const std::vector<LogField>& fields = {},
                    std::source_location location = std::source_location::current());

private:
    TransactionLogger() = default;

    bool ensureInitializedLocked();
    bool ensureWritableFileLocked(std::size_t nextLineBytes);
    bool openNewFileLocked();
    bool rotateIfNeededLocked(std::size_t nextLineBytes);
    bool writeHeaderLocked();
    bool writePayloadLocked(const std::string& payload);

    std::filesystem::path logDirectory_;
    std::filesystem::path currentFile_;
    std::uintmax_t maxFileBytes_ = 1024ULL * 1024ULL * 1024ULL;
    std::uintmax_t currentFileBytes_ = 0;
    unsigned int fileSequence_ = 0;
    bool initialized_ = false;
    bool disabled_ = false;
    std::ofstream file_;
    std::atomic<int> minimumSeverity_{20};
    mutable std::mutex mutex_;
};
