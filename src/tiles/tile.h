/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/rect.h"
#include "effect/globals.h"
#include "virtualdesktops.h"
#include "window.h"
#include <kwin_export.h>

#include <QObject>

namespace KWin
{

class Gravity;
class TileManager;
class VirtualDesktop;
class Window;

/*!
 * \qmluncreatabletype Tile
 * \inqmlmodule org.kde.kwin
 *
 */
class KWIN_EXPORT Tile : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    /*!
     * \qmlproperty RectF Tile::relativeGeometry
     */
    Q_PROPERTY(KWin::RectF relativeGeometry READ relativeGeometry WRITE setRelativeGeometry NOTIFY relativeGeometryChanged)

    /*!
     * \qmlproperty RectF Tile::absoluteGeometry
     */
    Q_PROPERTY(KWin::RectF absoluteGeometry READ absoluteGeometry NOTIFY absoluteGeometryChanged)

    /*!
     * \qmlproperty RectF Tile::absoluteGeometryInScreen
     */
    Q_PROPERTY(KWin::RectF absoluteGeometryInScreen READ absoluteGeometryInScreen NOTIFY absoluteGeometryChanged)

    /*!
     * \qmlproperty real Tile::padding
     */
    Q_PROPERTY(qreal padding READ padding WRITE setPadding NOTIFY paddingChanged)

    /*!
     * \qmlproperty size Tile::minimumSize
     */
    Q_PROPERTY(QSizeF minimumSize READ minimumSize WRITE setMinimumSize NOTIFY minimumSizeChanged)

    /*!
     * \qmlproperty int Tile::positionInLayout
     */
    Q_PROPERTY(int positionInLayout READ row NOTIFY rowChanged)

    /*!
     * \qmlproperty Tile Tile::parent
     */
    Q_PROPERTY(KWin::Tile *parent READ parentTile CONSTANT)

    /*!
     * \qmlproperty list<Tile> Tile::tiles
     */
    Q_PROPERTY(QList<KWin::Tile *> tiles READ childTiles NOTIFY childTilesChanged)

    /*!
     * \qmlproperty list<Window> Tile::windows
     */
    Q_PROPERTY(QList<KWin::Window *> windows READ windows NOTIFY windowsChanged)

    /*!
     * \qmlproperty bool Tile::isLayout
     */
    Q_PROPERTY(bool isLayout READ isLayout NOTIFY isLayoutChanged)

    /*!
     * \qmlproperty bool Tile::canBeRemoved
     */
    Q_PROPERTY(bool canBeRemoved READ canBeRemoved CONSTANT)

public:
    enum class LayoutDirection {
        Floating = 0,
        Horizontal = 1,
        Vertical = 2,
    };
    Q_ENUM(LayoutDirection)

    explicit Tile(TileManager *tiling, Tile *parentItem = nullptr);
    ~Tile();

    VirtualDesktop *desktop() const;
    bool isActive() const;
    bool isTransient() const;

    void setGeometryFromWindow(const RectF &geom);
    void setGeometryFromAbsolute(const RectF &geom);
    virtual void setRelativeGeometry(const RectF &geom);

    virtual bool supportsResizeGravity(Gravity gravity);

    /*!
     * Geometry of the tile in units between 0 and 1 relative to the screen geometry
     */
    RectF relativeGeometry() const;

    /*!
     * Geometry of the tile in absolute coordinates
     */
    RectF absoluteGeometry() const;

    /*!
     * Geometry of the tile in absolute coordinates, but in screen coordinates,
     * ie the top left corner of rootTile always at 0,0
     */
    RectF absoluteGeometryInScreen() const;

    /*!
     * Absolute geometry minus the padding and reserved areas such as panels
     */
    RectF windowGeometry() const;

    /*!
     * Absolute geometry minus the padding and reserved areas such as panels
     */
    RectF maximizedWindowGeometry() const;

    /*!
     * Which edges of the tile touches an edge of the screen
     */
    Qt::Edges anchors() const;

    bool isRoot() const;
    bool isLayout() const;
    bool canBeRemoved() const;

    qreal padding() const;
    void setPadding(qreal padding);

    QSizeF minimumSize() const;
    void setMinimumSize(const QSizeF &size);

    QuickTileMode quickTileMode() const;
    void setQuickTileMode(QuickTileMode mode);

    Tile *rootTile() const;

    /*!
     * All tiles directly children of this tile
     */
    QList<Tile *> childTiles() const;

    /*!
     * All tiles descendant of this tile, recursive
     */
    QList<Tile *> descendants() const;

    /*!
     * Visit all tiles descendant of this tile, recursive
     */
    void visitDescendants(std::function<void(Tile *child)> callback);

    void resizeFromGravity(Gravity gravity, int x_root, int y_root);

    /*!
     * \qmlmethod void Tile::resizeByPixels(real delta, enumeration edge)
     */
    Q_INVOKABLE void resizeByPixels(qreal delta, Qt::Edge edge);

    /*!
     * \qmlmethod void Tile::manage(Window window)
     */
    Q_INVOKABLE bool manage(KWin::Window *window);

    /*!
     * \qmlmethod void Tile::unmanage(Window window)
     */
    Q_INVOKABLE bool unmanage(KWin::Window *window);
    void forget(Window *window);
    QList<KWin::Window *> windows() const;

    int row() const;
    int childCount() const;
    Tile *childTile(int row);
    Tile *nextSibling() const;
    Tile *previousSibling() const;
    Tile *parentTile() const;
    TileManager *manager() const;

    void destroyChild(Tile *tile);

    template<class T>
    T *createChildAt(const RectF &relativeGeometry, int position)
    {
        if (isTransient()) {
            return nullptr;
        }
        T *t = new T(m_tiling, static_cast<T *>(this));
        t->setRelativeGeometry(relativeGeometry);
        insertChild(position, t);
        return t;
    }

Q_SIGNALS:
    void activeChanged(bool active);
    void relativeGeometryChanged();
    void absoluteGeometryChanged();
    void windowGeometryChanged();
    void paddingChanged(qreal padding);
    void minimumSizeChanged(const QSizeF &size);
    void rowChanged(int row);
    void isLayoutChanged(bool isLayout);
    void childTilesChanged();
    void windowsChanged();

    /*!
     * \qmlsignal windowAdded(Window window)
     */
    void windowAdded(KWin::Window *window);

    /*!
     * \qmlsignal windowRemoved(Window window)
     */
    void windowRemoved(KWin::Window *window);

protected:
    enum class Type {
        Native,
        Transient,
    };

    // Transient admission/layout stays private; membership release delegates to its owner.
    Tile(TileManager *tiling, Tile *parentItem, Type type);
    virtual bool releaseTransientWindow(Window *window);

    void insertChild(int position, Tile *item);
    void removeChild(Tile *child);

    void add(Window *window);
    bool remove(Window *window);

    QList<Tile *> m_children;
    QList<Window *> m_windows;
    Tile *m_parentTile;

    VirtualDesktop *m_desktop = nullptr;
    TileManager *m_tiling;
    RectF m_relativeGeometry;
    QSizeF m_minimumSize = QSizeF(0.15, 0.15);
    QuickTileMode m_quickTileMode = QuickTileFlag::None;
    qreal m_padding = 4.0;

private:
    friend class TilesTest;
    // The compatibility setter consumes the private steps with its own continuation.
    friend class Window;
    // Guarded membership steps for an ordinary ingress: refusals (the backend first, a transient
    // tile, an unresizable or applet window, a window not on this desktop, a destination that is
    // not live, the follow-up association step's read-only preflight, membership headroom) before
    // any mutation; an existing member or a miss as an operation-owned no-op without a stamp; the
    // evacuation over a worklist collected without callbacks, with the Window retained and the
    // destination's context frozen and validated after every notification; each membership write
    // sealed by its own checkpoint; then the association step chaining the last checkpoint. After
    // the first mutation every stop is Superseded; nothing is rolled back and nothing falls
    // through. A destroyed destination is never read as explicit null.
    // reserve: membership increments a caller's follow-up (the compatibility setter's cleanup of the
    // original membership) will need, held back from this step's headroom: zero or one, nothing
    // general-purpose.
    [[nodiscard]] Window::AssignmentStepResult manageForAssignment(Window *window, const TileAssignmentFence &before, Window::OwnedProvenance owned = {}, quint64 reserve = 0);
    [[nodiscard]] Window::AssignmentStepResult unmanageForAssignment(Window *window, const TileAssignmentFence &before, Window::OwnedProvenance owned = {});
    [[nodiscard]] Window::AssignmentStepResult forgetForAssignment(Window *window, const TileAssignmentFence &before, Window::OwnedProvenance owned = {});
    Window::AssignmentStepResult membershipNoOp(Window *window, Window::OwnedProvenance owned) const;
    // The destination's frozen context as a predicate captured by value: this tile, its manager,
    // desktop and parent by guarded identity, its liveness, the active decision, the target
    // geometry, and the window's desktop list; composed into every continuation of a management
    // and into the provenance handed to the association step.
    std::function<bool()> assignmentContext(Window *window) const;
    void removeForAssignment(Window *window, std::unique_ptr<TileAssignmentFence> &checkpoint, const std::function<bool()> &current);
    bool destinationIsLive() const;
    // The public bodies after their rejections, verbatim: selected by the public entry points when
    // the guarded step refused without effect, or at the exhaustion edge.
    bool manageLegacy(Window *window);
    bool unmanageLegacy(Window *window);
    void forgetLegacy(Window *window);
    const Type m_type;
};

} // namespace KWin
