/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tilemanager.h"
#include "core/backendoutput.h"
#include "quicktile.h"
#include "transienttilecontroller.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>

namespace KWin
{

QDebug operator<<(QDebug debug, const TileManager *tileManager)
{
    if (tileManager) {
        QList<Tile *> tiles({tileManager->rootTile(VirtualDesktopManager::self()->currentDesktop(tileManager->output()))});
        QList<Tile *> tilePath;
        QString indent(QStringLiteral("|-"));
        debug << tileManager->metaObject()->className() << '(' << static_cast<const void *>(tileManager) << ')' << '\n';
        while (!tiles.isEmpty()) {
            auto *tile = tiles.last();
            tiles.pop_back();
            debug << indent << qobject_cast<CustomTile *>(tile) << '\n';
            if (tile->childCount() > 0) {
                tiles.append(tile->childTiles());
                tilePath.append(tile->childTiles().first());
                indent.prepend(QStringLiteral("| "));
            }
            if (!tilePath.isEmpty() && tile == tilePath.last()) {
                tilePath.pop_back();
                indent.remove(0, 2);
            }
        }

    } else {
        debug << "Tile(0x0)";
    }
    return debug;
}

TileManager::TileManager(LogicalOutput *parent)
    : QObject(parent)
    , m_output(parent)
{
    m_saveTimer = std::make_unique<QTimer>(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(2000);
    connect(m_saveTimer.get(), &QTimer::timeout, this, &TileManager::saveSettings);

    auto addDesktop = [this](VirtualDesktop *desk) {
        RootTile *rootTile = new RootTile(this, desk);
        m_rootTiles[desk] = rootTile;
        m_quickRootTiles[desk] = new QuickRootTile(this, desk);

        rootTile->setRelativeGeometry(RectF(0, 0, 1, 1));
        connect(rootTile, &CustomTile::paddingChanged, m_saveTimer.get(), static_cast<void (QTimer::*)()>(&QTimer::start));
        connect(rootTile, &CustomTile::layoutModified, m_saveTimer.get(), static_cast<void (QTimer::*)()>(&QTimer::start));

        readSettings(rootTile);
    };

    for (VirtualDesktop *desk : VirtualDesktopManager::self()->desktops()) {
        addDesktop(desk);
    }

    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopAdded, this, addDesktop);
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopRemoved,
            this, [this](VirtualDesktop *desk) {
        const QPointer<TileManager> guard(this);
        const bool acquired = !m_drainingDesktops.contains(desk);
        m_drainingDesktops.insert(desk);
        const auto release = qScopeGuard([guard, desk, acquired]() {
            if (guard && acquired) {
                guard->m_drainingDesktops.remove(desk);
            }
        });
        drainTransientRoots(desk);
        if (!guard) {
            return;
        }
        delete m_rootTiles.take(desk);
        if (!guard) {
            return;
        }
        delete m_quickRootTiles.take(desk);
    });
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::currentChanged,
            this, [this](VirtualDesktop *oldDesk, VirtualDesktop *newDesk, LogicalOutput *output) {
        if (output != m_output) {
            return;
        }
        Q_EMIT rootTileChanged(rootTile());
        Q_EMIT modelChanged(model());
    });
}

TileManager::~TileManager()
{
    prepareForRemoval();
    m_tearingDown = true;
}

bool TileManager::canRegisterTransientRoot(VirtualDesktop *desktop) const
{
    return !m_tearingDown && !m_transientRegistrationClosed && desktop && !m_drainingDesktops.contains(desktop)
        && m_output && workspace()->outputs().contains(m_output) && VirtualDesktopManager::self()->desktops().contains(desktop)
        && m_rootTiles.contains(desktop) && m_quickRootTiles.contains(desktop) && !m_transientRoots.contains(desktop);
}

std::shared_ptr<TileManager::TransientRegistration> TileManager::registerTransientRoot(Tile *root, TransientTileController *controller, quint64 generation)
{
    if (!root || !controller || !generation || !canRegisterTransientRoot(root->desktop()) || !root->isTransient()
        || root->manager() != this || root->parentTile() || root->parent() != controller
        || controller->rootTile() != root || controller->generation() != generation || m_transientLifetime.contains(root)) {
        return {};
    }
    auto registration = std::make_shared<TransientRegistration>(TransientRegistration{root, controller, root->desktop(), generation});
    m_transientRoots.insert(root->desktop(), registration);
    m_transientLifetime.insert(root, registration);
    connect(root, &QObject::destroyed, this, [this, root, registration]() {
        if (m_transientRoots.value(registration->desktop) == registration) {
            m_transientRoots.remove(registration->desktop);
        }
        if (m_transientLifetime.value(root) == registration) {
            m_transientLifetime.remove(root);
        }
    });
    return registration;
}

bool TileManager::unregisterTransientRoot(VirtualDesktop *desktop, Tile *root, TransientTileController *controller, quint64 generation)
{
    const auto it = m_transientRoots.constFind(desktop);
    if (it == m_transientRoots.cend() || (*it)->root != root || (*it)->controller != controller || (*it)->generation != generation) {
        return false;
    }
    // Lookup ends now; weak lifetime tracking remains until actual QObject destruction.
    m_transientRoots.remove(desktop);
    return true;
}

void TileManager::closeTransientRegistration()
{
    m_transientRegistrationClosed = true;
}

void TileManager::prepareForRemoval()
{
    closeTransientRegistration();
    drainTransientRoots();
}

void TileManager::disposeTransientRoot(const std::shared_ptr<TransientRegistration> &registration)
{
    if (!registration->root || registration->destroying || registration->draining) {
        return;
    }
    registration->draining = true;
    const auto finish = qScopeGuard([registration]() {
        registration->draining = false;
    });
    const auto root = registration->root;
    const auto controller = registration->controller;
    if (controller && controller->rootTile() == root && controller->generation() == registration->generation) {
        const auto result = controller->clearTree(registration->generation);
        if (root && !registration->destroying && result != TransientTileController::Result::Accepted) {
            qCWarning(KWIN_CORE) << "Transient root needs fallback disposal after controller cleanup" << int(result);
        }
    }
    if (root && !registration->destroying) {
        delete root.data();
    }
}

void TileManager::drainTransientRoots(VirtualDesktop *desktop)
{
    const QPointer<TileManager> guard(this);
    const bool acquired = desktop && !m_drainingDesktops.contains(desktop);
    if (acquired) {
        m_drainingDesktops.insert(desktop);
    }
    const auto release = qScopeGuard([guard, desktop, acquired]() {
        if (guard && acquired) {
            guard->m_drainingDesktops.remove(desktop);
        }
    });
    QList<std::shared_ptr<TransientRegistration>> roots;
    for (auto it = m_transientRoots.begin(); it != m_transientRoots.end();) {
        if (!desktop || it.key() == desktop) {
            roots.append(it.value());
            it = m_transientRoots.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &registration : std::as_const(m_transientLifetime)) {
        if ((!desktop || registration->desktop == desktop) && !roots.contains(registration)) {
            roots.append(registration);
        }
    }
    // The captured metadata owns no QObject and remains usable after a callback deletes us.
    const auto cleanup = qScopeGuard([roots = std::move(roots)]() {
        for (const auto &registration : roots) {
            disposeTransientRoot(registration);
        }
    });
}

bool TileManager::tearingDown() const
{
    return m_tearingDown;
}

LogicalOutput *TileManager::output() const
{
    return m_output;
}

Tile *TileManager::bestTileForPosition(qreal x, qreal y)
{
    return rootTile()->pick(QPointF(x, y));
}

RootTile *TileManager::rootTile(VirtualDesktop *desktop) const
{
    return m_rootTiles.value(desktop);
}

RootTile *TileManager::rootTile() const
{
    return m_rootTiles.value(VirtualDesktopManager::self()->currentDesktop(m_output));
}

QuickRootTile *TileManager::quickRootTile() const
{
    return m_quickRootTiles.value(VirtualDesktopManager::self()->currentDesktop(m_output));
}

QuickRootTile *TileManager::quickRootTile(VirtualDesktop *desktop) const
{
    return m_quickRootTiles.value(desktop);
}

Tile *TileManager::quickTile(QuickTileMode mode) const
{
    return quickRootTile()->tileForMode(mode);
}

TileModel *TileManager::model() const
{
    return rootTile()->model();
}

Tile *TileManager::tileForWindow(Window *window, VirtualDesktop *desktop)
{
    if (!window || !desktop || m_tearingDown) {
        return nullptr;
    }
    const auto registration = m_transientRoots.value(desktop);
    if (registration && registration->root && !registration->destroying && !registration->draining && registration->controller
        && registration->controller->rootTile() == registration->root && registration->controller->generation() == registration->generation) {
        Tile *owner = nullptr;
        registration->root->visitDescendants([&](Tile *tile) {
            if (tile->windows().contains(window)) {
                owner = tile;
            }
        });
        if (owner) {
            return owner;
        }
    }
    const auto quick = m_quickRootTiles.value(desktop);
    const auto manual = m_rootTiles.value(desktop);
    if (!quick || !manual) {
        return nullptr;
    }
    Tile *owner = quick->tileForWindow(window);
    if (owner) {
        return owner;
    }
    return manual->tileForWindow(window);
}

void TileManager::forgetWindow(Window *window, VirtualDesktop *desktop)
{
    if (!window) {
        return;
    }
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Window> target(window);
    const QPointer<TileManager> guard(this);
    QList<QPointer<VirtualDesktop>> desktops;
    if (desktop) {
        desktops.append(desktop);
    } else {
        for (auto desk : VirtualDesktopManager::self()->desktops()) {
            desktops.append(desk);
        }
    }
    for (const auto &desk : desktops) {
        if (!guard || !target) {
            return;
        }
        if (!desk) {
            continue;
        }
        Tile *owner = guard->tileForWindow(target, desk);
        if (owner) {
            owner->forget(target);
            if (!guard || !target || target->isDeleted()) {
                return;
            }
        }
    }
}

Tile::LayoutDirection strToLayoutDirection(const QString &dir)
{
    if (dir == QLatin1StringView("horizontal")) {
        return Tile::LayoutDirection::Horizontal;
    } else if (dir == QLatin1StringView("vertical")) {
        return Tile::LayoutDirection::Vertical;
    } else {
        return Tile::LayoutDirection::Floating;
    }
}

CustomTile *TileManager::parseTilingJSon(const QJsonValue &val, const RectF &availableArea, CustomTile *parentTile)
{
    if (availableArea.isEmpty()) {
        return nullptr;
    }

    if (val.isObject()) {
        const auto &obj = val.toObject();
        CustomTile *createdTile = nullptr;

        if (parentTile->layoutDirection() == Tile::LayoutDirection::Horizontal) {
            RectF rect = availableArea;
            const auto width = obj.value(QStringLiteral("width"));
            if (width.isDouble()) {
                rect.setWidth(std::min(width.toDouble(), availableArea.width()));
            }
            if (!rect.isEmpty()) {
                createdTile = parentTile->createChildAt(rect, parentTile->layoutDirection(), parentTile->childCount());
            }

        } else if (parentTile->layoutDirection() == Tile::LayoutDirection::Vertical) {
            RectF rect = availableArea;
            const auto height = obj.value(QStringLiteral("height"));
            if (height.isDouble()) {
                rect.setHeight(std::min(height.toDouble(), availableArea.height()));
            }
            if (!rect.isEmpty()) {
                createdTile = parentTile->createChildAt(rect, parentTile->layoutDirection(), parentTile->childCount());
            }

        } else if (parentTile->layoutDirection() == Tile::LayoutDirection::Floating) {
            RectF rect(0, 0, 1, 1);
            rect = RectF(obj.value(QStringLiteral("x")).toDouble(),
                         obj.value(QStringLiteral("y")).toDouble(),
                         obj.value(QStringLiteral("width")).toDouble(),
                         obj.value(QStringLiteral("height")).toDouble());

            if (!rect.isEmpty()) {
                createdTile = parentTile->createChildAt(rect, parentTile->layoutDirection(), parentTile->childCount());
            }
        }

        if (createdTile && obj.contains(QStringLiteral("tiles"))) {
            // It's a layout
            const auto arr = obj.value(QStringLiteral("tiles"));
            const auto direction = obj.value(QStringLiteral("layoutDirection"));
            // Ignore arrays with only a single item in it
            if (arr.isArray() && arr.toArray().count() > 0) {
                const Tile::LayoutDirection dir = strToLayoutDirection(direction.toString());
                createdTile->setLayoutDirection(dir);
                parseTilingJSon(arr, createdTile->relativeGeometry(), createdTile);
            }
        }
        return createdTile;
    } else if (val.isArray()) {
        const auto arr = val.toArray();
        auto avail = availableArea;
        for (auto it = arr.cbegin(); it != arr.cend(); it++) {
            if ((*it).isObject()) {
                auto *tile = parseTilingJSon(*it, avail, parentTile);
                if (tile && parentTile->layoutDirection() == Tile::LayoutDirection::Horizontal) {
                    avail.setLeft(tile->relativeGeometry().right());
                } else if (tile && parentTile->layoutDirection() == Tile::LayoutDirection::Vertical) {
                    avail.setTop(tile->relativeGeometry().bottom());
                }
            }
        }
        // make sure the children fill exactly the parent, eventually enlarging the last
        if (parentTile->layoutDirection() != Tile::LayoutDirection::Floating
            && parentTile->childCount() > 0) {
            auto *last = parentTile->childTile(parentTile->childCount() - 1);
            auto geom = last->relativeGeometry();
            geom.setRight(parentTile->relativeGeometry().right());
            last->setRelativeGeometry(geom);
        }
        return nullptr;
    }
    return nullptr;
}

// this is the old output UUID format from Plasma 6.3
// to not reset the config on updates, attempt to load the settings with this uuid too!
static QString generateOutputId(LogicalOutput *output)
{
    static const QUuid urlNs = QUuid("6ba7b811-9dad-11d1-80b4-00c04fd430c8"); // NameSpace_URL
    static const QUuid kwinNs = QUuid::createUuidV5(urlNs, QStringLiteral("https://kwin.kde.org/o/"));

    const QString payload = QStringList{output->name(), output->backendOutput()->eisaId(), output->model(), output->serialNumber()}.join(':');
    return QUuid::createUuidV5(kwinNs, payload).toString(QUuid::StringFormat::WithoutBraces);
}

void TileManager::readSettings(RootTile *rootTile)
{
    KConfigGroup cg = kwinApp()->config()->group(QStringLiteral("Tiling"));
    qreal padding = cg.readEntry("padding", 4); // Fall back to the "global" padding set previously
    cg.deleteEntry("padding"); // clean up residual value, TODO clean after 6.7 or 6.8 is out
    VirtualDesktop *desk = rootTile->desktop();
    KConfigGroup desktopCg = KConfigGroup(&cg, desk->id());
    cg = KConfigGroup(&desktopCg, m_output->uuid());
    padding = cg.readEntry("padding", padding);

    Q_ASSERT(m_rootTiles.contains(desk));

    auto createDefaultSetup = [](RootTile *rootTile) {
        Q_ASSERT(rootTile->childCount() == 0);
        // If empty create an horizontal 3 columns layout
        rootTile->setLayoutDirection(Tile::LayoutDirection::Horizontal);
        rootTile->split(Tile::LayoutDirection::Horizontal);
        static_cast<CustomTile *>(rootTile->childTile(0))->split(Tile::LayoutDirection::Horizontal);
        Q_ASSERT(rootTile->childCount() == 3);
        // Resize middle column, the other two will be auto resized accordingly
        rootTile->childTile(1)->setRelativeGeometry({0.25, 0.0, 0.5, 1.0});
    };

    QJsonParseError error;
    auto tiles = cg.readEntry("tiles", QByteArray());
    if (tiles.isEmpty()) {
        cg = KConfigGroup(&desktopCg, generateOutputId(m_output));
        tiles = cg.readEntry("tiles", QByteArray());
        if (tiles.isEmpty()) {
            qCDebug(KWIN_CORE) << "Empty tiles configuration for monitor" << m_output->uuid() << ":"
                               << "Creating default setup";
            createDefaultSetup(rootTile);
            return;
        }
    }
    QJsonDocument doc = QJsonDocument::fromJson(tiles, &error);

    if (error.error != QJsonParseError::NoError) {
        qCWarning(KWIN_CORE) << "Parse error in tiles configuration for monitor" << m_output->uuid() << ":" << error.errorString() << "Creating default setup";
        createDefaultSetup(rootTile);
        return;
    }

    if (doc.object().contains(QStringLiteral("tiles"))) {
        const auto arr = doc.object().value(QStringLiteral("tiles"));
        if (arr.isArray() && arr.toArray().count() > 0) {
            rootTile->setLayoutDirection(strToLayoutDirection(doc.object().value(QStringLiteral("layoutDirection")).toString()));
            parseTilingJSon(arr, RectF(0, 0, 1, 1), rootTile);
        }
    }

    rootTile->setPadding(padding);
}

QJsonObject TileManager::tileToJSon(CustomTile *tile)
{
    QJsonObject obj;

    auto *parentTile = static_cast<CustomTile *>(tile->parentTile());

    // Exclude the root and the two children
    if (parentTile) {
        switch (parentTile->layoutDirection()) {
        case Tile::LayoutDirection::Horizontal:
            obj[QStringLiteral("width")] = tile->relativeGeometry().width();
            break;
        case Tile::LayoutDirection::Vertical:
            obj[QStringLiteral("height")] = tile->relativeGeometry().height();
            break;
        case Tile::LayoutDirection::Floating:
        default:
            obj[QStringLiteral("x")] = tile->relativeGeometry().x();
            obj[QStringLiteral("y")] = tile->relativeGeometry().y();
            obj[QStringLiteral("width")] = tile->relativeGeometry().width();
            obj[QStringLiteral("height")] = tile->relativeGeometry().height();
        }
    }

    if (tile->isLayout()) {
        switch (tile->layoutDirection()) {
        case Tile::LayoutDirection::Horizontal:
            obj[QStringLiteral("layoutDirection")] = QStringLiteral("horizontal");
            break;
        case Tile::LayoutDirection::Vertical:
            obj[QStringLiteral("layoutDirection")] = QStringLiteral("vertical");
            break;
        case Tile::LayoutDirection::Floating:
        default:
            obj[QStringLiteral("layoutDirection")] = QStringLiteral("floating");
        }

        QJsonArray tiles;
        const int nChildren = tile->childCount();
        for (int i = 0; i < nChildren; ++i) {
            tiles.append(tileToJSon(static_cast<CustomTile *>(tile->childTile(i))));
        }
        obj[QStringLiteral("tiles")] = tiles;
    }

    return obj;
}

void TileManager::saveSettings()
{
    KConfigGroup cg = kwinApp()->config()->group(QStringLiteral("Tiling"));

    for (auto it = m_rootTiles.constBegin(); it != m_rootTiles.constEnd(); it++) {
        VirtualDesktop *desk = it.key();
        RootTile *rootTile = it.value();
        auto obj = tileToJSon(rootTile);
        QJsonDocument doc(obj);
        KConfigGroup tileGroup(&cg, desk->id());
        tileGroup = KConfigGroup(&tileGroup, m_output->uuid());
        tileGroup.writeEntry("tiles", doc.toJson(QJsonDocument::Compact));
        tileGroup.writeEntry("padding", rootTile->padding());
    }
    cg.sync(); // FIXME: less frequent?
}

} // namespace KWin

#include "moc_tilemanager.cpp"
