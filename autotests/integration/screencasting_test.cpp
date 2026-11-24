/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2023 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "compositor.h"
#include "core/output.h"
#include "core/renderloop.h"
#include "generic_scene_opengl_test.h"
#include "opengl/glplatform.h"
#include "pointer_input.h"
#include "scene/workspacescene.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KWayland/Client/output.h>
#include <KWayland/Client/subsurface.h>
#include <KWayland/Client/surface.h>
#include <PipeWireSourceStream>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QScreen>
#include <QTemporaryDir>

#define QCOMPAREIMG(actual, expected, id)                                                        \
    {                                                                                            \
        if ((actual) != (expected)) {                                                            \
            const auto actualFile = QStringLiteral("appium_artifact_actual_%1.png").arg(id);     \
            const auto expectedFile = QStringLiteral("appium_artifact_expected_%1.png").arg(id); \
            (actual).save(actualFile);                                                           \
            (expected).save(expectedFile);                                                       \
            qDebug() << "Generated failed file" << actualFile << expectedFile;                   \
        }                                                                                        \
        QCOMPARE(actual, expected);                                                              \
    }

namespace KWin
{

class ScreencastingTest : public GenericSceneOpenGLTest
{
    Q_OBJECT

public:
    ScreencastingTest()
        : GenericSceneOpenGLTest()
    {
        auto wrap = [this](const QString &process, const QStringList &arguments = {}) {
            // Make sure PipeWire is running. If it's already running it will just exit
            QProcess *p = new QProcess(this);
            p->setProcessChannelMode(QProcess::MergedChannels);
            p->setArguments(arguments);
            connect(this, &QObject::destroyed, p, [p] {
                p->terminate();
                p->waitForFinished();
                p->kill();
            });
            connect(p, &QProcess::errorOccurred, p, [p](auto status) {
                qDebug() << "error" << status << p->program();
            });
            connect(p, &QProcess::finished, p, [p](int code, auto status) {
                if (code != 0) {
                    qDebug() << p->readAll();
                }
                qDebug() << "finished" << code << status << p->program();
            });
            p->setProgram(process);
            p->start();
        };

        // If I run this outside the CI, it breaks the system's pipewire
        if (qgetenv("KDECI_BUILD") == "TRUE") {
            wrap("pipewire");
            wrap("dbus-launch", {"wireplumber"});
        }
    }
private Q_SLOTS:
    void init();
    void testWindowCasting();
    void testWindowWithPopup();
    void testWindowWithPopupDynamic();
    void testOutputCasting();
    void testOutputSkipsRequesterWindow();

private:
    std::optional<QImage> oneFrameAndClose(Test::ScreencastingStreamV1 *stream);
};

void ScreencastingTest::init()
{
    if (qgetenv("KDECI_BUILD") == "TRUE") {
        QSKIP("CI has pipewire 1.2 that has known process callback issues"); // TODO: Remove it later when CI ships pipewire 1.2 with the fix
    }

    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::ScreencastingV1 | Test::AdditionalWaylandInterface::PresentationTime));
    QVERIFY(KWin::Test::screencasting());
    Cursors::self()->hideCursor();
}

std::optional<QImage> ScreencastingTest::oneFrameAndClose(Test::ScreencastingStreamV1 *stream)
{
    Q_ASSERT(stream);
    PipeWireSourceStream pwStream;
    qDebug() << "start" << stream;
    connect(stream, &Test::ScreencastingStreamV1::failed, qGuiApp, [](const QString &error) {
        qDebug() << "stream failed with error" << error;
        Q_ASSERT(false);
    });
    connect(stream, &Test::ScreencastingStreamV1::closed, qGuiApp, [&pwStream] {
        pwStream.setActive(false);
    });
    connect(stream, &Test::ScreencastingStreamV1::created, qGuiApp, [&pwStream](quint64 serial) {
        pwStream.createStream(serial, 0);
    });

    std::optional<QImage> img;
    connect(&pwStream, &PipeWireSourceStream::frameReceived, qGuiApp, [&img](const PipeWireFrame &frame) {
        if (frame.dataFrame) {
            img = frame.dataFrame->toImage();
        }
    });

    QSignalSpy spy(&pwStream, &PipeWireSourceStream::frameReceived);
    if (!spy.wait()) {
        qDebug() << "Did not receive any frames";
    }
    pwStream.stopStreaming();
    return img;
}

void ScreencastingTest::testWindowCasting()
{
    QImage sourceImage(QSize(30, 10), QImage::Format_RGBA8888_Premultiplied);
    sourceImage.fill(Qt::red);
    Test::XdgToplevelWindow window;
    QVERIFY(window.show(sourceImage));

    auto stream = KWin::Test::screencasting()->createWindowStream(window.m_window->internalId().toString(), QtWayland::zkde_screencast_unstable_v1::pointer_hidden);

    std::optional<QImage> img = oneFrameAndClose(stream);
    QVERIFY(img);
    img->convertTo(sourceImage.format());
    QCOMPAREIMG(*img, sourceImage, QLatin1StringView("window_cast"));
}

void ScreencastingTest::testWindowWithPopup()
{
    QImage windowImage(QSize(30, 10), QImage::Format_RGBA8888_Premultiplied);
    windowImage.fill(Qt::red);
    Test::XdgToplevelWindow window;
    QVERIFY(window.show(windowImage));

    QSize popupSize(5, 5);
    Rect anchoRect(0, 0, 1, 1);
    std::unique_ptr<KWayland::Client::Surface> popupSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(popupSize.width(), popupSize.height());
    positioner->set_anchor_rect(anchoRect.x(), anchoRect.y(), anchoRect.width(), anchoRect.height());
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(popupSurface.get(), window.m_toplevel->xdgSurface(), positioner.get()));

    QImage popupImage(popupSize, QImage::Format_RGBA8888_Premultiplied);
    popupImage.fill(Qt::blue);
    auto popupWindow = Test::renderAndWaitForShown(popupSurface.get(), popupImage);
    QVERIFY(popupWindow);

    std::unique_ptr<Test::ScreencastingStreamV1> stream(KWin::Test::screencasting()->createWindowStream(window.m_window->internalId().toString(), Test::ScreencastingV1::pointer_hidden));

    QImage expectedImage = windowImage;
    QPainter(&expectedImage).drawImage(anchoRect.bottomRight(), popupImage);

    std::optional<QImage> img = oneFrameAndClose(stream.get());
    QVERIFY(img);
    img->convertTo(expectedImage.format());
    QCOMPAREIMG(*img, expectedImage, QLatin1StringView("window_popup"));
}

void ScreencastingTest::testWindowWithPopupDynamic()
{
    QImage windowImage(QSize(30, 10), QImage::Format_RGBA8888_Premultiplied);
    windowImage.fill(Qt::red);
    Test::XdgToplevelWindow window;
    QVERIFY(window.show(windowImage));

    std::unique_ptr<Test::ScreencastingStreamV1> stream(KWin::Test::screencasting()->createWindowStream(window.m_window->internalId().toString(), Test::ScreencastingV1::pointer_hidden));

    PipeWireSourceStream pwStream;
    connect(stream.get(), &Test::ScreencastingStreamV1::failed, qGuiApp, [](const QString &error) {
        QFAIL("Creating stream failed: " + error.toUtf8());
    });
    connect(stream.get(), &Test::ScreencastingStreamV1::closed, qGuiApp, [&pwStream] {
        pwStream.setActive(false);
    });
    QSignalSpy createdSpy(stream.get(), &Test::ScreencastingStreamV1::created);
    QVERIFY(createdSpy.wait());
    pwStream.createStream(createdSpy.first().first().value<quint64>(), 0);

    auto fetchNextImageFrame = [&pwStream](QImage::Format format) {
        QSignalSpy frameSpy(&pwStream, &PipeWireSourceStream::frameReceived);
        if (!frameSpy.wait()) {
            Q_ASSERT_X(false, "", "no frame received");
            return QImage();
        }
        auto frame = frameSpy.first().first().value<PipeWireFrame>();
        return frame.dataFrame->toImage().convertedTo(format);
    };

    QImage expectedImage = windowImage;
    auto image = fetchNextImageFrame(expectedImage.format());
    QCOMPAREIMG(image, expectedImage, "dynamic_initial");

    QSize popupSize(5, 5);
    Rect anchoRect(0, 0, 1, 1);
    std::unique_ptr<KWayland::Client::Surface> popupSurface(Test::createSurface());
    std::unique_ptr<Test::XdgPositioner> positioner(Test::createXdgPositioner());
    positioner->set_size(popupSize.width(), popupSize.height());
    positioner->set_anchor_rect(anchoRect.x(), anchoRect.y(), anchoRect.width(), anchoRect.height());
    positioner->set_gravity(Test::XdgPositioner::gravity_bottom_right);
    std::unique_ptr<Test::XdgPopup> popup(Test::createXdgPopupSurface(popupSurface.get(), window.m_toplevel->xdgSurface(), positioner.get()));

    QImage popupImage(popupSize, QImage::Format_RGBA8888_Premultiplied);
    popupImage.fill(Qt::blue);
    auto popupWindow = Test::renderAndWaitForShown(popupSurface.get(), popupImage);
    QVERIFY(popupWindow);

    expectedImage = windowImage;
    QPainter(&expectedImage).drawImage(anchoRect.bottomRight(), popupImage);
    image = fetchNextImageFrame(expectedImage.format());
    QCOMPAREIMG(image, expectedImage, "dynamic_popup");

    popup.reset();

    expectedImage = windowImage;
    image = fetchNextImageFrame(expectedImage.format());
    QCOMPAREIMG(image, expectedImage, "dynamic_popup_closed");
}

static QImage outputCastingImage(const QSize &size)
{
    QImage sourceImage(size, QImage::Format_RGBA8888_Premultiplied);
    sourceImage.fill(Qt::green);
    {
        QPainter p(&sourceImage);
        p.drawRect(100, 100, 100, 100);
    }
    return sourceImage;
}

void ScreencastingTest::testOutputCasting()
{
    const QPointer<KWayland::Client::Output> theOutput = Test::waylandOutputs().value(0);
    QTRY_VERIFY(theOutput && !theOutput->pixelSize().isEmpty() && !theOutput->name().isEmpty());
    const QString outputName = theOutput->name();
    const QPointer<LogicalOutput> output = workspace()->findOutput(outputName);
    QVERIFY(output);
    const QImage sourceImage = outputCastingImage(theOutput->pixelSize());
    const QTemporaryDir fixtures;
    QVERIFY(fixtures.isValid());
    const QString imagePath = fixtures.filePath(QStringLiteral("output.png"));
    QVERIFY(sourceImage.save(imagePath));

    // Output casts omit the requesting client's windows. Map the image from a
    // separate, display-only client, connected directly to this test compositor.
    QPointer<Window> window;
    bool windowClosed = false;
    QProcess client;
    const auto stopClient = [&client] {
        if (client.state() == QProcess::NotRunning) {
            return true;
        }
        client.terminate();
        if (client.waitForFinished(1000)) {
            return true;
        }
        client.kill();
        return client.waitForFinished(5000);
    };
    const auto cleanup = qScopeGuard([&] {
        stopClient();
        QTest::qWaitFor([&] {
            return !window || windowClosed;
        });
    });

    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("WAYLAND_SOCKET"));
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), waylandServer()->socketName());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    environment.insert(QStringLiteral("QT_WAYLAND_SHELL_INTEGRATION"), QStringLiteral("xdg-shell"));
    client.setProcessEnvironment(environment);
    client.setProcessChannelMode(QProcess::ForwardedChannels);

    const auto watchWindow = [&](Window *candidate) {
        const auto pid = client.processId();
        if (window || windowClosed || !pid || candidate->pid() != pid || candidate->isDeleted()) {
            return;
        }
        window = candidate;
        connect(candidate, &Window::closed, &client, [&windowClosed] {
            windowClosed = true;
        });
    };
    connect(workspace(), &Workspace::windowAdded, &client, watchWindow);
    client.start(QCoreApplication::applicationDirPath() + QStringLiteral("/screencast-output-client"), {imagePath, outputName});
    QVERIFY2(client.waitForStarted(5000), qPrintable(client.errorString()));
    // Also check windows already mapped when QProcess reports the started PID.
    for (Window *candidate : workspace()->windows()) {
        watchWindow(candidate);
    }
    QTRY_VERIFY(window || windowClosed || client.state() == QProcess::NotRunning);
    const auto clientWindowIsOnOutput = [&] {
        return window && !windowClosed && client.state() == QProcess::Running && output && theOutput && window->output() == output.data();
    };
    QVERIFY2(clientWindowIsOnOutput(), "The display client did not stay mapped on the requested output");
    QVERIFY(window->isFullScreen());
    QCOMPARE(window->frameGeometry(), RectF(output->geometry()));

    auto backendOutput = output->backendOutput();
    QVERIFY(backendOutput);
    QSignalSpy presented(backendOutput->renderLoop(), &RenderLoop::framePresented);
    kwinApp()->scene()->addRepaintFull();
    QVERIFY(presented.wait());
    QVERIFY2(clientWindowIsOnOutput(), "The display client disappeared or left the requested output before capture");

    {
        std::unique_ptr<Test::ScreencastingStreamV1> stream(Test::screencasting()->createOutputStream(theOutput->output(), Test::ScreencastingV1::pointer_hidden));
        std::optional<QImage> img = oneFrameAndClose(stream.get());
        QVERIFY2(clientWindowIsOnOutput(), "The display client disappeared or left the requested output during capture");
        QVERIFY(img);
        img->convertTo(sourceImage.format());
        QCOMPAREIMG(*img, sourceImage, QLatin1StringView("output_cast"));
    }

    QVERIFY2(stopClient(), "The display client did not terminate");
    QTRY_VERIFY(windowClosed);
}

void ScreencastingTest::testOutputSkipsRequesterWindow()
{
    const QPointer<KWayland::Client::Output> theOutput = Test::waylandOutputs().value(0);
    QTRY_VERIFY(theOutput && !theOutput->pixelSize().isEmpty() && !theOutput->name().isEmpty());
    const QPointer<LogicalOutput> output = workspace()->findOutput(theOutput->name());
    QVERIFY(output);
    const QImage sourceImage = outputCastingImage(theOutput->pixelSize());
    Test::XdgToplevelWindow window{[theOutput](Test::XdgToplevel *toplevel) {
        if (theOutput) {
            toplevel->set_fullscreen(theOutput->output());
        }
    }};

    QVERIFY(window.show(sourceImage));
    const QPointer<Window> mappedWindow = window.m_window;
    const auto clearDeletedWindow = qScopeGuard([&] {
        if (!mappedWindow) {
            window.m_window = nullptr;
        }
    });
    QVERIFY(mappedWindow && output && theOutput);
    QCOMPARE(mappedWindow->pid(), QCoreApplication::applicationPid());
    QVERIFY(mappedWindow->isFullScreen());
    QCOMPARE(mappedWindow->output(), output.data());
    QCOMPARE(mappedWindow->frameGeometry(), RectF(output->geometry()));

    QVERIFY(window.presentWait());
    QVERIFY(mappedWindow && output && theOutput);
    QCOMPARE(mappedWindow->output(), output.data());

    const std::unique_ptr<Test::ScreencastingStreamV1> stream(Test::screencasting()->createOutputStream(theOutput->output(), Test::ScreencastingV1::pointer_hidden));
    std::optional<QImage> img = oneFrameAndClose(stream.get());
    QVERIFY(mappedWindow && output && theOutput);
    QCOMPARE(mappedWindow->output(), output.data());
    QVERIFY(img);
    QImage expectedImage(sourceImage.size(), sourceImage.format());
    expectedImage.fill(Qt::transparent);
    img->convertTo(expectedImage.format());
    QCOMPAREIMG(*img, expectedImage, QLatin1StringView("output_skip_requester"));
}

}

WAYLANDTEST_MAIN(KWin::ScreencastingTest)
#include "screencasting_test.moc"
