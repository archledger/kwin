/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QWidget>

#include <algorithm>

class OutputFixtureWindow : public QWidget
{
public:
    explicit OutputFixtureWindow(const QImage &image)
        : m_image(image)
    {
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter(this).drawImage(QPoint(), m_image);
    }

private:
    const QImage m_image;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 3) {
        qCritical("Expected an image path and a Wayland output name");
        return 1;
    }
    const QImage image(arguments.at(1));
    if (image.isNull()) {
        qCritical() << "Could not load the synthetic image:" << arguments.at(1);
        return 1;
    }
    const auto screens = app.screens();
    const auto screen = std::ranges::find(screens, arguments.at(2), &QScreen::name);
    if (screen == screens.end()) {
        qCritical() << "Could not find the requested Wayland output:" << arguments.at(2);
        return 1;
    }

    OutputFixtureWindow window(image);
    window.setScreen(*screen);
    window.showFullScreen();
    return app.exec();
}
