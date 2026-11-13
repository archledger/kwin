/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "customtile.h"
#include "quicktile.h"
#include "scripting/tilemodel.h"
#include "tile.h"
#include "utils/common.h"
#include <kwin_export.h>

#include <QAbstractItemModel>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <QJsonValue>
#include <memory>

class QTimer;

// The transient tiling's types, declared in a namespace block of the fork's own: a line of
// ours among upstream's declarations below is a merge conflict with every edit beside it.
namespace KWin
{

class TransientTile;
class TransientTileController;
class Workspace;

}

namespace KWin
{

class LogicalOutput;
class Tile;
class TileModel;

/**
 * Custom tiling zones management per output.
 */
class KWIN_EXPORT TileManager : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(KWin::Tile *rootTile READ rootTile NOTIFY rootTileChanged)
    Q_PROPERTY(TileModel *model READ model NOTIFY modelChanged)

public:
    explicit TileManager(LogicalOutput *parent = nullptr);
    ~TileManager() override;

    bool tearingDown() const;

    LogicalOutput *output() const;

    Q_INVOKABLE KWin::Tile *bestTileForPosition(qreal x, qreal y); // For scripting
    RootTile *rootTile(VirtualDesktop *desktop) const;
    RootTile *rootTile() const;
    QuickRootTile *quickRootTile(VirtualDesktop *desktop) const;
    QuickRootTile *quickRootTile() const;
    KWin::Tile *quickTile(QuickTileMode mode) const;

    TileModel *model() const;

    Tile *tileForWindow(Window *window, VirtualDesktop *desktop);
    void forgetWindow(Window *window, VirtualDesktop *desktop);
    // Native lifecycle boundary; not a scripting entry point.
    void prepareForRemoval();

Q_SIGNALS:
    void tileRemoved(KWin::Tile *tile);
    void rootTileChanged(CustomTile *rootTile);
    void modelChanged(TileModel *model);

private:
    friend class TransientTile;
    friend class TransientTileController;
    friend class Workspace;

    struct TransientRegistration
    {
        QPointer<Tile> root;
        QPointer<TransientTileController> controller;
        VirtualDesktop *desktop;
        quint64 generation;
        bool destroying = false;
        bool draining = false;
    };

    bool canRegisterTransientRoot(VirtualDesktop *desktop) const;
    std::shared_ptr<TransientRegistration> registerTransientRoot(Tile *root, TransientTileController *controller, quint64 generation);
    bool unregisterTransientRoot(VirtualDesktop *desktop, Tile *root, TransientTileController *controller, quint64 generation);
    void closeTransientRegistration();
    void drainTransientRoots(VirtualDesktop *desktop = nullptr);
    static void disposeTransientRoot(const std::shared_ptr<TransientRegistration> &registration);

    void readSettings(RootTile *rootTile);
    void saveSettings();
    QJsonObject tileToJSon(CustomTile *parentTile);
    CustomTile *parseTilingJSon(const QJsonValue &val, const RectF &availableArea, CustomTile *parentTile);

    Q_DISABLE_COPY(TileManager)

    LogicalOutput *m_output = nullptr;
    std::unique_ptr<QTimer> m_saveTimer;

    QHash<VirtualDesktop *, RootTile *> m_rootTiles;
    QHash<VirtualDesktop *, QuickRootTile *> m_quickRootTiles;
    QHash<VirtualDesktop *, std::shared_ptr<TransientRegistration>> m_transientRoots;
    QHash<Tile *, std::shared_ptr<TransientRegistration>> m_transientLifetime;
    QSet<VirtualDesktop *> m_drainingDesktops;
    bool m_transientRegistrationClosed = false;

    bool m_tearingDown = false;
    friend class CustomTile;
};

KWIN_EXPORT QDebug operator<<(QDebug debug, const TileManager *tileManager);

} // namespace KWin
