#pragma once

#include <QString>

class CrashReporter final
{
public:
    CrashReporter() = delete;

    static void install();
    static QString crashDirectory();
    static QString lastError();

    // Writes a small diagnostic dump without terminating the process. This is
    // used by the reliability smoke test and by --crash-test automation.
    static bool writeTestDump(QString* dumpPath = nullptr);

    // Raises an unhandled exception so the installed filter can be exercised.
    [[noreturn]] static void simulateCrash();
};
