#include "../CrashReporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FFmpegQtPlayer"));
    QCoreApplication::setApplicationName(QStringLiteral("CrashReporterSmoke"));

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        std::fprintf(stderr, "Cannot create a temporary crash directory.\n");
        return 70;
    }
    qputenv("FFMPEGQT_CRASH_DIR", temporaryDirectory.path().toLocal8Bit());
    CrashReporter::install();

    QString dumpPath;
    if (!CrashReporter::writeTestDump(&dumpPath)) {
        std::fprintf(
            stderr,
            "Crash reporter failed: %s\nDirectory: %s\n",
            CrashReporter::lastError().toUtf8().constData(),
            CrashReporter::crashDirectory().toUtf8().constData());
        return 1;
    }
    const QFileInfo dumpInfo(dumpPath);
    const QFileInfo metadataInfo(dumpPath + QStringLiteral(".txt"));
    if (!dumpInfo.exists() || dumpInfo.size() <= 0
        || !metadataInfo.exists() || metadataInfo.size() <= 0) {
        std::fprintf(stderr, "Crash dump or metadata is missing.\n");
        return 2;
    }

    std::printf(
        "dump=%s bytes=%lld metadata=%s\n",
        dumpInfo.absoluteFilePath().toUtf8().constData(),
        static_cast<long long>(dumpInfo.size()),
        metadataInfo.absoluteFilePath().toUtf8().constData());
    return 0;
}
