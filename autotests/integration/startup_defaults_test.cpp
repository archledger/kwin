/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwin_wayland_test.h"
#include "options.h"

#include <KConfigGroup>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace KWin;

class StartupTestApplication : public Application
{
public:
    StartupTestApplication(int &argc, char **argv)
        : Application(argc, argv)
    {
    }

protected:
    void performStartup() override
    {
        qFatal("The defaults test must not start a compositor");
    }
};

class StartupDefaultsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOptionsConfigured_data();
    void testOptionsConfigured();
    void testVirtualInputDefaults();
};

void StartupDefaultsTest::testOptionsConfigured_data()
{
    QTest::addColumn<int>("configured");
    QTest::newRow("default") << -1;
    QTest::newRow("false") << 0;
    QTest::newRow("true") << 1;
}

void StartupDefaultsTest::testOptionsConfigured()
{
    QFETCH(int, configured);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto config = KSharedConfig::openConfig(directory.filePath(QStringLiteral("kwinrc")), KConfig::SimpleConfig);
    if (configured >= 0) {
        KConfigGroup(config, QStringLiteral("Windows")).writeEntry("ElectricBorderAllScreenCorner", bool(configured));
    }
    QVERIFY(config->sync());
    const auto oldConfig = kwinApp()->config();
    kwinApp()->setConfig(config);
    const auto restoreConfig = qScopeGuard([oldConfig]() {
        kwinApp()->setConfig(oldConfig);
    });
    const bool expected = configured != 0;
    Options tested(nullptr);
    QCOMPARE(tested.electricBorderAllScreenCorner(), expected);
    QSignalSpy changed(&tested, &Options::electricBorderAllScreenCornerChanged);
    tested.setElectricBorderAllScreenCorner(expected);
    QCOMPARE(changed.count(), 0);
    tested.setElectricBorderAllScreenCorner(!expected);
    QCOMPARE(tested.electricBorderAllScreenCorner(), !expected);
    QCOMPARE(changed.count(), 1);
    tested.setElectricBorderAllScreenCorner(!expected);
    QCOMPARE(changed.count(), 1);
    tested.setElectricBorderAllScreenCorner(expected);
    QCOMPARE(tested.electricBorderAllScreenCorner(), expected);
    QCOMPARE(changed.count(), 2);
}

void StartupDefaultsTest::testVirtualInputDefaults()
{
    Test::VirtualInputDevice device;
    QVERIFY(!device.isTabletTool());
    QVERIFY(!device.tabletToolIsRelative());
    device.setTabletTool(true);
    QVERIFY(!device.tabletToolIsRelative());
    device.setTabletToolIsRelative(true);
    QVERIFY(device.tabletToolIsRelative());
    device.setTabletToolIsRelative(true);
    QVERIFY(device.tabletToolIsRelative());
    device.setTabletToolIsRelative(false);
    QVERIFY(!device.tabletToolIsRelative());
}

int main(int argc, char **argv)
{
    if (!qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"))) {
        qFatal("Could not select the offscreen test platform");
    }
    QStandardPaths::setTestModeEnabled(true);
    StartupTestApplication app(argc, argv);
    StartupDefaultsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "startup_defaults_test.moc"
