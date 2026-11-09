/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "input.h"
#include "kwin_wayland_test.h"
#include "tabletmodemanager.h"
#include "wayland_server.h"

#include <KConfigGroup>
#include <QSignalSpy>

using namespace KWin;

class StartupTabletSwitch : public Test::VirtualInputDevice
{
public:
    bool isTabletModeSwitch() const override
    {
        return true;
    }
};

class TabletModeStartupTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testInitiallyAvailable();
    void testSwitchState_data();
    void testSwitchState();

private:
    StartupTabletSwitch m_switch;
    bool m_registered = false;
    bool m_beforeManager = false;
};

void TabletModeStartupTest::initTestCase()
{
    auto app = qobject_cast<WaylandTestApplication *>(kwinApp());
    QVERIFY(app);
    KConfigGroup(kwinApp()->config(), QStringLiteral("Input")).writeEntry("TabletMode", "auto");
    connect(app, &WaylandTestApplication::inputDevicesCreated, this, [this]() {
        m_beforeManager = input() && !kwinApp()->tabletModeManager();
        input()->addInputDevice(&m_switch);
        m_registered = true;
    });
    QVERIFY(waylandServer()->init(qAppName()));
    app->start();
    QVERIFY(m_registered);
    QVERIFY(m_beforeManager);
    QVERIFY(input()->hasTabletModeSwitch());
}

void TabletModeStartupTest::cleanupTestCase()
{
    if (m_registered) {
        input()->removeInputDevice(&m_switch);
    }
}

void TabletModeStartupTest::testInitiallyAvailable()
{
    auto manager = kwinApp()->tabletModeManager();
    QVERIFY(manager);
    QCOMPARE(manager->configuredMode(), TabletModeManager::ConfiguredMode::Auto);
    QVERIFY(manager->isTabletModeAvailable());
    QVERIFY(manager->property("tabletModeAvailable").toBool());
}

void TabletModeStartupTest::testSwitchState_data()
{
    QTest::addColumn<bool>("tablet");
    QTest::newRow("on") << true;
    QTest::newRow("off") << false;
}

void TabletModeStartupTest::testSwitchState()
{
    QFETCH(bool, tablet);
    auto manager = kwinApp()->tabletModeManager();
    Q_EMIT m_switch.switchToggle(tablet ? SwitchState::Off : SwitchState::On, std::chrono::microseconds(1), &m_switch);
    QSignalSpy changed(manager, &TabletModeManager::tabletModeChanged);
    Q_EMIT m_switch.switchToggle(tablet ? SwitchState::On : SwitchState::Off, std::chrono::microseconds(2), &m_switch);
    QCOMPARE(manager->isTablet(), tablet);
    QCOMPARE(manager->effectiveTabletMode(), tablet);
    QCOMPARE(changed.count(), 1);
    QVERIFY(manager->isTabletModeAvailable());
}

WAYLANDTEST_MAIN(TabletModeStartupTest)
#include "tabletmode_startup_test.moc"
