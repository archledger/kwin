/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/rect.h"
#include "window.h"
#include <kwin_export.h>

#include <QObject>
#include <QPointer>
#include <QUuid>

namespace KWin
{

class Tile;
class TileManager;
class TransientTile;
class TransientPlacementWriter;
class VirtualDesktop;
class Window;

// Internal native authority; no scripting API or production instance is exposed yet.
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
        Ineligible,
        Refused,
    };

    explicit TransientTileController(QObject *parent = nullptr);
    ~TransientTileController() override;

    Result createTree(TileManager *manager, VirtualDesktop *desktop, const RectF &geometry);
    Result setGeometry(quint64 generation, const RectF &geometry);
    Result clearTree(quint64 generation);
    Result admit(quint64 generation, Window *window, TransientPlacementWriter *writer, const QUuid &provider,
                 const QString &key, quint64 providerRevision, quint64 mapRevision);

    quint64 generation() const;
    Tile *rootTile() const;
    Tile *tile() const;

Q_SIGNALS:
    void treeChanged();

private:
    friend class TransientTile;
    friend class Window;
    class Mutation;
    class Request;
    class Release;

    void resetTree();
    void nodeAboutToBeDestroyed(TransientTile *node);
    bool admissionCurrent(Window *window, quint64 generation) const;
    // With a result, the release's own writes are inside a checkpoint taken before its
    // notifications and validated once after its finalization; see Window::revokeTransientOwnership.
    bool releaseWindow(Window *window, quint64 generation, Window::AssignmentStepResult *guarded = nullptr);
    void clearAdmission();

    QPointer<TransientTile> m_root;
    QPointer<TransientTile> m_tile;
    QPointer<TileManager> m_manager;
    QPointer<VirtualDesktop> m_desktop;
    QMetaObject::Connection m_managerDestroyed;
    QMetaObject::Connection m_desktopDestroyed;
    QMetaObject::Connection m_windowClosed;
    QMetaObject::Connection m_providerRetired;
    QMetaObject::Connection m_keyConflict;
    QPointer<Window> m_window;
    QString m_key;
    QString m_activity;
    quint64 m_ownershipGeneration = 0;
    bool m_membershipPublished = false;
    quint64 m_generation = 0;
    bool m_mutating = false;
    bool m_shuttingDown = false;
};

} // namespace KWin
