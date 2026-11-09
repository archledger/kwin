/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "main.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <cstring>

using namespace KWin;

class ShutdownTestApplication : public Application
{
public:
    ShutdownTestApplication(int &argc, char **argv)
        : Application(argc, argv)
    {
    }

protected:
    void performStartup() override
    {
        // Exercise Application::start() without creating a compositor or workspace.
    }
};

static int runChild(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    {
        ShutdownTestApplication app(argc, argv);
        app.start();
        QTimer::singleShot(10, &app, &QCoreApplication::quit);
        app.exec();
        std::puts("hang-test: destruction begins");
        std::fflush(stdout);
    }
    std::puts("hang-test: destruction finished");
    std::fflush(stdout);
    return 0;
}

class HangDetectorShutdownTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testShutdown_data();
    void testShutdown();
};

void HangDetectorShutdownTest::testShutdown_data()
{
    QTest::addColumn<QString>("timeout");
    QTest::newRow("disabled") << QStringLiteral("0");
    QTest::newRow("enabled") << QStringLiteral("1000");
}

void HangDetectorShutdownTest::testShutdown()
{
    QFETCH(QString, timeout);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString runtime = directory.filePath(QStringLiteral("runtime"));
    QVERIFY(QDir().mkdir(runtime));
    QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HOME"), directory.path());
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), directory.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), directory.filePath(QStringLiteral("cache")));
    environment.insert(QStringLiteral("XDG_DATA_HOME"), directory.filePath(QStringLiteral("data")));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_STYLE_OVERRIDE"), QStringLiteral("Fusion"));
    environment.insert(QStringLiteral("KWIN_HANG_DETECTION_TIMEOUT"), timeout);

    for (int attempt = 0; attempt < 16; ++attempt) {
        QProcess child;
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--shutdown-child")});
        QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
        const bool finished = child.state() == QProcess::NotRunning || child.waitForFinished(5000);
        if (!finished) {
            child.kill();
            QVERIFY2(child.state() == QProcess::NotRunning || child.waitForFinished(1000), "Could not reap timed-out shutdown child");
        }
        const QByteArray output = child.readAll();
        const QByteArray context = QByteArray("attempt ") + QByteArray::number(attempt) + '\n' + output;
        QVERIFY2(finished, context.constData());
        QVERIFY2(child.exitStatus() == QProcess::NormalExit, context.constData());
        QVERIFY2(child.exitCode() == 0, context.constData());
        QVERIFY2(output.contains("hang-test: destruction begins"), context.constData());
        QVERIFY2(output.contains("hang-test: destruction finished"), context.constData());
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--shutdown-child") == 0) {
        return runChild(argc, argv);
    }
    QCoreApplication app(argc, argv);
    HangDetectorShutdownTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "hang_detector_shutdown_test.moc"
