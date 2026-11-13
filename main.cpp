#include "MainWindow.h"
#include "CrashReporter.h"

#include <QApplication>
#include <QStyleFactory>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("FFmpegQtPlayer"));
    QApplication::setApplicationName(QStringLiteral("FFmpeg Qt Player"));
    QApplication::setApplicationVersion(QStringLiteral("2.0.0"));
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    CrashReporter::install();

    MainWindow window;
    window.show();
    QStringList arguments = application.arguments().mid(1);
    if (arguments.removeAll(QStringLiteral("--crash-test")) > 0) {
        QTimer::singleShot(300, &application, [] {
            CrashReporter::simulateCrash();
        });
    } else {
        window.openCommandLineMedia(arguments);
    }
    return application.exec();
}
