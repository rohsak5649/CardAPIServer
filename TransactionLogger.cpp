#include "TransactionLogger.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <utility>

namespace {

struct LogContext {
    std::string uuid;
    std::string channel;
};

thread_local LogContext g_logContext;

constexpr int kWriteAttempts = 3;
constexpr int kTraceSeverity = 0;
constexpr int kDebugSeverity = 10;
constexpr int kInfoSeverity = 20;
constexpr int kWarnSeverity = 30;
constexpr int kErrorSeverity = 40;

std::string upperAscii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        result.push_back(static_cast<char>(std::toupper(ch)));
    }
    return result;
}

int severityOf(std::string_view level) {
    const std::string upper = upperAscii(level);
    if (upper == "TRACE") return kTraceSeverity;
    if (upper == "DEBUG") return kDebugSeverity;
    if (upper == "INFO") return kInfoSeverity;
    if (upper == "WARN" || upper == "WARNING") return kWarnSeverity;
    if (upper == "ERROR" || upper == "FATAL") return kErrorSeverity;
    return kInfoSeverity;
}

int configuredMinimumSeverity() {
    const char* configured = std::getenv("TRANSACTION_LOG_LEVEL");
    if (configured == nullptr || configured[0] == '\0') {
        return kInfoSeverity;
    }
    return severityOf(configured);
}

bool hasField(const std::vector<LogField>& fields, std::string_view key) {
    for (const auto& field : fields) {
        if (field.key == key) {
            return true;
        }
    }
    return false;
}

void appendSourceLocation(std::vector<LogField>& fields,
                          const std::source_location& location) {
    if (!hasField(fields, "sourceFile")) {
        fields.push_back({"sourceFile", location.file_name()});
    }
    if (!hasField(fields, "sourceLine")) {
        fields.push_back({"sourceLine", std::to_string(location.line())});
    }
    if (!hasField(fields, "sourceFunction")) {
        fields.push_back({"sourceFunction", location.function_name()});
    }
}

std::tm localTime(std::time_t value) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    return tm;
}

std::string timestampForLine() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = localTime(raw);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

std::string timestampForFile() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = localTime(raw);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

std::string escapeText(std::string_view value) {
    std::ostringstream oss;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':  oss << "\\\""; break;   // preserve JSON quotes properly
            case '\\': oss << "\\\\"; break;   // preserve backslash
            case '\b': break;
            case '\f': break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (ch >= 0x20) {
                    oss << static_cast<char>(ch);
                }
        }
    }
    return oss.str();
}


std::string fieldOrDash(std::string_view value) {
    return value.empty() ? std::string("-") : std::string(value);
}

std::string formatFieldValue(std::string_view value) {
    const std::string escaped = escapeText(value);
    if (escaped.empty()) {
        return "\"\"";
    }

    bool needsQuotes = false;
    for (const unsigned char ch : escaped) {
        if (std::isspace(ch) || ch == '|' || ch == '=') {
            needsQuotes = true;
            break;
        }
    }
    return needsQuotes ? "\"" + escaped + "\"" : escaped;
}

std::vector<LogField> withFunction(std::string_view functionName,
                                   const std::vector<LogField>& fields) {
    std::vector<LogField> merged;
    merged.reserve(fields.size() + 1);
    merged.push_back({"function", std::string(functionName)});
    merged.insert(merged.end(), fields.begin(), fields.end());
    return merged;
}

long long elapsedMsSince(std::chrono::steady_clock::time_point startedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
}

} // namespace

TransactionLogger::ScopedContext::ScopedContext(std::string uuid,
                                                std::string channel)
    : previousUuid_(g_logContext.uuid),
      previousChannel_(g_logContext.channel) {
    g_logContext.uuid = std::move(uuid);
    g_logContext.channel = std::move(channel);
}

TransactionLogger::ScopedContext::~ScopedContext() {
    g_logContext.uuid = std::move(previousUuid_);
    g_logContext.channel = std::move(previousChannel_);
}

TransactionLogger::ScopedFunctionTrace::ScopedFunctionTrace(
    std::string functionName,
    std::vector<LogField> fields,
    std::source_location location)
    : functionName_(std::move(functionName)),
      sourceFile_(location.file_name()),
      sourceLine_(location.line()),
      sourceFunction_(location.function_name()),
      startedAt_(std::chrono::steady_clock::now()),
      exceptionCount_(std::uncaught_exceptions()) {
    std::vector<LogField> merged = withFunction(functionName_, fields);
    appendSourceLocation(merged, location);
    TransactionLogger::instance().logCurrent(
        "TRACE", "function_enter", "Entering function",
        merged);
}

TransactionLogger::ScopedFunctionTrace::~ScopedFunctionTrace() {
    if (completed_) {
        return;
    }

    std::vector<LogField> fields{
        {"function", functionName_},
        {"durationMs", std::to_string(elapsedMsSince(startedAt_))},
        {"sourceFile", sourceFile_},
        {"sourceLine", std::to_string(sourceLine_)},
        {"sourceFunction", sourceFunction_}
    };

    if (std::uncaught_exceptions() > exceptionCount_) {
        TransactionLogger::instance().logCurrent(
            "ERROR", "function_exception", "Function exited by exception", fields);
        return;
    }

    TransactionLogger::instance().logCurrent(
        "TRACE", "function_exit", "Leaving function", fields);
}

void TransactionLogger::ScopedFunctionTrace::checkpoint(
    std::string_view step,
    std::string_view message,
    const std::vector<LogField>& fields,
    std::source_location location) const {
    std::vector<LogField> merged = withFunction(functionName_, fields);
    merged.push_back({"step", std::string(step)});
    appendSourceLocation(merged, location);
    TransactionLogger::instance().logCurrent(
        "DEBUG", "function_step", message, merged);
}

void TransactionLogger::ScopedFunctionTrace::fail(
    std::string_view reason,
    const std::vector<LogField>& fields,
    std::source_location location) {
    if (completed_) {
        return;
    }

    std::vector<LogField> merged = withFunction(functionName_, fields);
    merged.push_back({"durationMs", std::to_string(elapsedMsSince(startedAt_))});
    merged.push_back({"reason", std::string(reason)});
    appendSourceLocation(merged, location);
    TransactionLogger::instance().logCurrent(
        "ERROR", "function_failed", "Function failed", merged);
    completed_ = true;
}

void TransactionLogger::ScopedFunctionTrace::success(
    const std::vector<LogField>& fields,
    std::source_location location) {
    if (completed_) {
        return;
    }

    std::vector<LogField> merged = withFunction(functionName_, fields);
    merged.push_back({"durationMs", std::to_string(elapsedMsSince(startedAt_))});
    appendSourceLocation(merged, location);
    TransactionLogger::instance().logCurrent(
        "TRACE", "function_exit", "Leaving function", merged);
    completed_ = true;
}

TransactionLogger& TransactionLogger::instance() {
    static TransactionLogger logger;
    return logger;
}

void TransactionLogger::initialize(std::filesystem::path logDirectory,
                                   std::uintmax_t maxFileBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return;
    }

    logDirectory_ = std::move(logDirectory);
    maxFileBytes_ = maxFileBytes;
    minimumSeverity_.store(configuredMinimumSeverity());

    if (!openNewFileLocked()) {
        std::cerr << "[TransactionLogger] waiting for writable log path: "
                  << logDirectory_.string() << '\n';
    }
}

void TransactionLogger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    initialized_ = false;
}

std::string TransactionLogger::generateUuid() const {
    // ── Thread-local RNG seeded once per thread ──────────────────────────────
    // Composite seed combines three independent entropy sources:
    //   1. std::random_device  — hardware entropy (two independent draws)
    //   2. std::this_thread::get_id() hash — unique per OS thread
    //   3. std::chrono::high_resolution_clock — nanosecond timestamp at init
    //
    // This eliminates the seed-collision risk present when random_device
    // alone is used: on entropy-starved systems (e.g. a freshly booted
    // container with 10 M threads launching simultaneously), multiple threads
    // can receive the same random_device seed and therefore produce identical
    // UUID sequences. The composite seed makes that scenario impossible.
    static thread_local std::mt19937_64 rng = [] {
        std::random_device rd;
        const auto tid = std::this_thread::get_id();
        const std::size_t tidHash = std::hash<std::thread::id>{}(tid);
        const auto ns =
            std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::seed_seq seed{
            rd(),                                   // hardware entropy #1
            rd(),                                   // hardware entropy #2 (independent draw)
            static_cast<std::uint32_t>(tidHash),          // thread-id low 32 bits
            static_cast<std::uint32_t>(tidHash >> 32),    // thread-id high 32 bits
            static_cast<std::uint32_t>(ns),               // timestamp low 32 bits
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(ns) >> 32) // timestamp high
        };
        return std::mt19937_64(seed);
    }();

    // unsigned short is the correct type for byte-range values [0,255].
    // The prior int distribution was functionally safe but semantically wrong.
    std::uniform_int_distribution<unsigned short> byteDist(0, 255);

    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(byteDist(rng));
    }

    // UUID v4: set version nibble (0100) and variant bits (10xx)
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

std::filesystem::path TransactionLogger::logDirectory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return logDirectory_.empty()
        ? std::filesystem::path("bin") / "debug" / "log"
        : logDirectory_;
}

std::string TransactionLogger::currentUuid() {
    return g_logContext.uuid;
}

std::string TransactionLogger::currentChannel() {
    return g_logContext.channel;
}

void TransactionLogger::log(std::string_view level,
                            std::string_view uuid,
                            std::string_view channel,
                            std::string_view event,
                            std::string_view message,
                            const std::vector<LogField>& fields,
                            std::source_location location) {
    const int severity = severityOf(level);
    if (severity < minimumSeverity_.load()) {
        return;
    }

    std::vector<LogField> sourceFields;
    const std::vector<LogField>* fieldsToWrite = &fields;
    if (severity >= kErrorSeverity && !hasField(fields, "sourceFile")) {
        sourceFields = fields;
        appendSourceLocation(sourceFields, location);
        fieldsToWrite = &sourceFields;
    }

    std::ostringstream line;
    line << timestampForLine()
         << " | " << fieldOrDash(level)
         << " | uuid=" << fieldOrDash(uuid)
         << " | channel=" << fieldOrDash(channel)
         << " | event=" << fieldOrDash(event)
         << " | message=\"" << escapeText(message) << "\"";

    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    line << " | thread=" << threadId.str();

    for (const auto& field : *fieldsToWrite) {
        line << " " << field.key << "=" << formatFieldValue(field.value);
    }
    line << "\n";

    const std::string payload = line.str();

    std::lock_guard<std::mutex> lock(mutex_);
    for (int attempt = 1; attempt <= kWriteAttempts; ++attempt) {
        if (!ensureWritableFileLocked(payload.size())) {
            continue;
        }
        if (writePayloadLocked(payload)) {
            return;
        }
        if (attempt < kWriteAttempts) {
            openNewFileLocked();
        }
    }

    std::cerr << "[TransactionLogger] failed to write log after "
              << kWriteAttempts << " attempts\n";
}

void TransactionLogger::logCurrent(std::string_view level,
                                   std::string_view event,
                                   std::string_view message,
                                   const std::vector<LogField>& fields,
                                   std::source_location location) {
    log(level, g_logContext.uuid, g_logContext.channel, event, message, fields,
        location);
}

bool TransactionLogger::ensureInitializedLocked() {
    if (disabled_) {
        return false;
    }
    if (initialized_ && file_.is_open()) {
        return true;
    }

    if (logDirectory_.empty()) {
        logDirectory_ = std::filesystem::path("bin") / "debug" / "log";
    }

    std::error_code ec;
    std::filesystem::create_directories(logDirectory_, ec);
    if (ec) {
        initialized_ = false;
        std::cerr << "[TransactionLogger] cannot create log directory "
                  << logDirectory_.string() << ": " << ec.message() << '\n';
        return false;
    }

    return openNewFileLocked();
}

bool TransactionLogger::ensureWritableFileLocked(std::size_t nextLineBytes) {
    if (!ensureInitializedLocked()) {
        return false;
    }

    std::error_code ec;
    const bool directoryExists = std::filesystem::exists(logDirectory_, ec);
    if (ec || !directoryExists) {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        initialized_ = false;
        currentFileBytes_ = 0;
        return ensureInitializedLocked();
    }

    ec.clear();
    if (!std::filesystem::is_directory(logDirectory_, ec) || ec) {
        initialized_ = false;
        currentFileBytes_ = 0;
        std::cerr << "[TransactionLogger] log path is not a directory: "
                  << logDirectory_.string() << '\n';
        return false;
    }

    ec.clear();
    const bool currentExists = !currentFile_.empty() &&
        std::filesystem::exists(currentFile_, ec);
    if (ec || !currentExists) {
        return openNewFileLocked();
    }

    ec.clear();
    const std::uintmax_t diskBytes = std::filesystem::file_size(currentFile_, ec);
    if (!ec) {
        if (diskBytes < currentFileBytes_) {
            if (file_.is_open()) {
                file_.flush();
                file_.close();
            }
            file_.clear();
            file_.open(currentFile_, std::ios::out | std::ios::app);
            if (!file_.is_open()) {
                initialized_ = false;
                currentFileBytes_ = 0;
                std::cerr << "[TransactionLogger] failed to reopen "
                          << currentFile_.string() << '\n';
                return false;
            }
        }
        currentFileBytes_ = diskBytes;
    }

    if (!file_.is_open()) {
        file_.clear();
        file_.open(currentFile_, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            initialized_ = false;
            currentFileBytes_ = 0;
            std::cerr << "[TransactionLogger] failed to open "
                      << currentFile_.string() << '\n';
            return false;
        }
    }

    if (currentFileBytes_ == 0 && !writeHeaderLocked()) {
        return false;
    }

    return rotateIfNeededLocked(nextLineBytes);
}

bool TransactionLogger::openNewFileLocked() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    file_.clear();
    // Close any previously held raw fd
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    std::error_code ec;
    std::filesystem::create_directories(logDirectory_, ec);
    if (ec) {
        initialized_ = false;
        currentFileBytes_ = 0;
        std::cerr << "[TransactionLogger] cannot create log directory "
                  << logDirectory_.string() << ": " << ec.message() << '\n';
        return false;
    }

    const std::string stamp = timestampForFile();
    std::filesystem::path candidate = logDirectory_ / ("log_" + stamp + ".log");
    while (std::filesystem::exists(candidate, ec)) {
        ++fileSequence_;
        candidate = logDirectory_ / ("log_" + stamp + "_" +
                                     std::to_string(fileSequence_) + ".log");
        ec.clear();
    }

    currentFile_ = candidate;
    currentFileBytes_ = 0;
    file_.open(currentFile_, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        initialized_ = false;
        std::cerr << "[TransactionLogger] failed to open "
                  << currentFile_.string() << '\n';
        return false;
    }

    // Open a parallel raw POSIX fd so writePayloadLocked can call ::fsync().
    // O_APPEND matches the ofstream open mode so both refer to the same file.
    fd_ = ::open(currentFile_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    // fd_ == -1 is tolerated — fsync simply won't be called; flush() still works.

    initialized_ = true;
    return writeHeaderLocked();
}

bool TransactionLogger::writeHeaderLocked() {
    const std::string header =
        "# Payment Switching Engine readable transaction log\n"
        "# Search any request with: uuid=<transactionUuid>\n"
        "# Format: time | level | uuid | channel | event | message | key=value...\n";
    file_ << header;
    file_.flush();
    if (!file_) {
        std::cerr << "[TransactionLogger] failed to write header to "
                  << currentFile_.string() << '\n';
        file_.clear();
        if (file_.is_open()) {
            file_.close();
        }
        initialized_ = false;
        currentFileBytes_ = 0;
        return false;
    }
    currentFileBytes_ += header.size();
    return true;
}

bool TransactionLogger::writePayloadLocked(const std::string& payload) {
    if (!file_.is_open()) {
        initialized_ = false;
        return false;
    }

    file_ << payload;
    // Two-stage flush:
    //   1. file_.flush()  — pushes C++ stream buffer into the OS page cache
    //   2. ::fsync(fd_)   — forces the OS to commit the page cache to disk
    // Together these guarantee every log line is physically on disk and visible
    // in the log viewer the instant writePayloadLocked() returns.
    file_.flush();
    if (fd_ >= 0) {
        ::fsync(fd_);
    }
    if (!file_) {
        std::cerr << "[TransactionLogger] failed to write to "
                  << currentFile_.string() << '\n';
        file_.clear();
        if (file_.is_open()) {
            file_.close();
        }
        initialized_ = false;
        currentFileBytes_ = 0;
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(currentFile_, ec) || ec) {
        std::cerr << "[TransactionLogger] active log file disappeared after write: "
                  << currentFile_.string() << '\n';
        if (file_.is_open()) {
            file_.close();
        }
        initialized_ = false;
        currentFileBytes_ = 0;
        return false;
    }

    currentFileBytes_ += payload.size();
    return true;
}

bool TransactionLogger::rotateIfNeededLocked(std::size_t nextLineBytes) {
    if (maxFileBytes_ == 0 ||
        currentFileBytes_ + static_cast<std::uintmax_t>(nextLineBytes) <= maxFileBytes_) {
        return true;
    }
    return openNewFileLocked();
}
