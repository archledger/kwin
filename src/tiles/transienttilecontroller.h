/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/rect.h"
#include <kwin_export.h>

#include <QObject>
#include <QPointer>

namespace KWin
{

class Tile;
class TileManager;
class TransientTile;
class VirtualDesktop;

// Internal empty-tree authority. This does not admit windows or expose a scripting API.
class KWIN_EXPORT TransientTileController : public QObject
{
    Q_OBJECT

public:
    enum class Result {
        Accepted,
        InvalidContext,
        InvalidGeometry,
        Busy,
        StaleGeneration,
        GenerationExhausted,
        Superseded,
    };

    explicit TransientTileController(QObject *parent = nullptr);
    ~TransientTileController() override;

    Result createTree(TileManager *manager, VirtualDesktop *desktop, const RectF &geometry);
    Result setGeometry(quint64 generation, const RectF &geometry);
    Result clearTree(quint64 generation);

    quint64 generation() const;
    Tile *rootTile() const;
    Tile *tile() const;

Q_SIGNALS:
    void treeChanged();

private:
    friend class TransientTile;
    class Mutation;

    void resetTree();
    void nodeAboutToBeDestroyed(TransientTile *node);

    QPointer<TransientTile> m_root;
    QPointer<TransientTile> m_tile;
    QPointer<TileManager> m_manager;
    QPointer<VirtualDesktop> m_desktop;
    QMetaObject::Connection m_managerDestroyed;
    QMetaObject::Connection m_desktopDestroyed;
    quint64 m_generation = 0;
    bool m_mutating = false;
    bool m_shuttingDown = false;
};

} // namespace KWin
