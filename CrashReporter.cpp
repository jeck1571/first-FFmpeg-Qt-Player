#include "CrashReporter.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

#include <atomic>
#include <cstdlib>

namespace {

#ifdef Q_OS_WIN

std::atomic_bool handlingCrash_{false};
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter_ = nullptr;
QString crashDirectory_;
QString lastError_;

QString timestampedDumpPath()
{
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    return QDir(crashDirectory_).filePath(
        QStringLiteral("FFmpegQtPlayer-%1.dmp").arg(stamp));
}

bool writeMetadata(const QString& dumpPath, const QString& reason, DWORD exceptionCode)
{
    QFile file(dumpPath + QStringLiteral(".txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    out << "FFmpegQtPlayer crash report\n";
    out << "UTC: " << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << '\n';
    out << "Reason: " << reason << '\n';
    out << "ExceptionCode: 0x" << QString::number(exceptionCode, 16).toUpper() << '\n';
    out << "ProcessId: " << static_cast<qulonglong>(GetCurrentProcessId()) << '\n';
    out << "ThreadId: " << static_cast<qulonglong>(GetCurrentThreadId()) << '\n';
    out << "Dump: " << QFileInfo(dumpPath).absoluteFilePath() << '\n';
    out.flush();
    return out.status() == QTextStream::Ok;
}

bool writeDump(EXCEPTION_POINTERS* exceptionPointers, const QString& reason,
               QString* dumpPath)
{
    if (crashDirectory_.isEmpty()) {
        lastError_ = QStringLiteral("Crash directory is empty.");
        return false;
    }
    if (!QDir().mkpath(crashDirectory_)) {
        lastError_ = QStringLiteral("Cannot create crash directory: %1")
                         .arg(crashDirectory_);
        return false;
    }
    const QString path = timestampedDumpPath();
    HANDLE file = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        lastError_ = QStringLiteral("Cannot create dump file (Win32 error %1).")
                         .arg(GetLastError());
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo);
    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        type,
        exceptionPointers ? &exceptionInfo : nullptr,
        nullptr,
        nullptr);
    const DWORD dumpError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        lastError_ = QStringLiteral("MiniDumpWriteDump failed (Win32 error %1).")
                         .arg(dumpError);
        DeleteFileW(reinterpret_cast<LPCWSTR>(path.utf16()));
        return false;
    }

    const DWORD exceptionCode = exceptionPointers && exceptionPointers->ExceptionRecord
        ? exceptionPointers->ExceptionRecord->ExceptionCode
        : 0;
    writeMetadata(path, reason, exceptionCode);
    if (dumpPath) {
        *dumpPath = path;
    }
    lastError_.clear();
    return true;
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers)
{
    if (!handlingCrash_.exchange(true, std::memory_order_acq_rel)) {
        QString dumpPath;
        writeDump(exceptionPointers, QStringLiteral("Unhandled exception"), &dumpPath);
        if (!dumpPath.isEmpty()) {
            OutputDebugStringW(
                reinterpret_cast<LPCWSTR>(
                    QStringLiteral("FFmpegQtPlayer crash dump: %1\n")
                        .arg(dumpPath)
                        .utf16()));
        }
    }

    if (previousFilter_ && previousFilter_ != unhandledExceptionFilter) {
        return previousFilter_(exceptionPointers);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif

} // namespace

void CrashReporter::install()
{
#ifdef Q_OS_WIN
    const QString overrideDirectory = qEnvironmentVariable(
        "FFMPEGQT_CRASH_DIR").trimmed();
    crashDirectory_ = overrideDirectory.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
              .filePath(QStringLiteral("crashes"))
        : QDir(overrideDirectory).absolutePath();
    QDir().mkpath(crashDirectory_);
    lastError_.clear();
    previousFilter_ = SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif
}

QString CrashReporter::crashDirectory()
{
#ifdef Q_OS_WIN
    return crashDirectory_;
#else
    return {};
#endif
}

QString CrashReporter::lastError()
{
#ifdef Q_OS_WIN
    return lastError_;
#else
    return QStringLiteral("Crash reporting is only implemented on Windows.");
#endif
}

bool CrashReporter::writeTestDump(QString* dumpPath)
{
#ifdef Q_OS_WIN
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0000001;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = reinterpret_cast<void*>(&CrashReporter::writeTestDump);
    EXCEPTION_POINTERS pointers{&record, &context};
    return writeDump(&pointers, QStringLiteral("Crash reporter smoke test"), dumpPath);
#else
    Q_UNUSED(dumpPath)
    return false;
#endif
}

void CrashReporter::simulateCrash()
{
#ifdef Q_OS_WIN
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
    std::abort();
#endif
}
