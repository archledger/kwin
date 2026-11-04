/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tile.h"
#include "core/output.h"
#include "effect/globals.h"
#include "tilemanager.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <cmath>

namespace KWin
{

Tile::Tile(TileManager *tiling, Tile *parent)
    : Tile(tiling, parent, Type::Native)
{
}

Tile::Tile(TileManager *tiling, Tile *parent, Type type)
    : QObject(parent)
    , m_parentTile(parent)
    , m_tiling(tiling)
    , m_quickTileMode(type == Type::Transient ? QuickTileFlag::Custom : QuickTileFlag::None)
    , m_type(type)
{
    if (m_parentTile) {
        m_padding = m_parentTile->padding();
        m_minimumSize = m_parentTile->m_minimumSize;
        m_desktop = m_parentTile->desktop();
    }
    connect(Workspace::self(), &Workspace::configChanged, this, &Tile::windowGeometryChanged);
    connect(Workspace::self(), &Workspace::windowRemoved, this, &Tile::unmanage);
}

Tile::~Tile()
{
    for (auto *t : std::as_const(m_children)) {
        // Prevents access upon child tiles destruction
        t->m_parentTile = nullptr;
    }
    if (m_parentTile) {
        m_parentTile->removeChild(this);
    }

    if (isTransient()) {
        // The controller must detach members before destruction, never reassign them manually.
        Q_ASSERT(m_windows.isEmpty());
        return;
    }

    if (m_tiling->tearingDown()) {
        return;
    }

    // TODO: Look for alternative designs to evacuate windows from removed tiles.
    auto windows = m_windows;
    for (auto *w : windows) {
        if (m_quickTileMode != QuickTileFlag::Custom) {
            unmanage(w);
        } else {
            Tile *tile = nullptr;
            if (RootTile *root = m_tiling->rootTile(m_desktop)) {
                tile = root->pick(w->moveResizeGeometry().center());
            }
            unmanage(w);
            if (tile) {
                tile->manage(w);
            }
        }
    }
}

VirtualDesktop *Tile::desktop() const
{
    return m_desktop;
}

bool Tile::isActive() const
{
    return m_desktop == VirtualDesktopManager::self()->currentDesktop(m_tiling->output());
}

bool Tile::isTransient() const
{
    return m_type == Type::Transient;
}

bool Tile::supportsResizeGravity(Gravity gravity)
{
    if (isTransient() || !m_parentTile) {
        return false;
    }

    switch (gravity) {
    case Gravity::Left:
        return m_relativeGeometry.left() > 0.0;
    case Gravity::Top:
        return m_relativeGeometry.top() > 0.0;
    case Gravity::Right:
        return m_relativeGeometry.right() < 1.0;
    case Gravity::Bottom:
        return m_relativeGeometry.bottom() < 1.0;
    case Gravity::TopLeft:
        return m_relativeGeometry.top() > 0.0 && m_relativeGeometry.left() > 0.0;
    case Gravity::TopRight:
        return m_relativeGeometry.top() > 0.0 && m_relativeGeometry.right() < 1.0;
    case Gravity::BottomLeft:
        return m_relativeGeometry.bottom() < 1.0 && m_relativeGeometry.left() > 0.0;
    case Gravity::BottomRight:
        return m_relativeGeometry.bottom() < 1.0 && m_relativeGeometry.right() < 1.0;
    case Gravity::Center:
    default:
        return false;
    }
}

void Tile::setGeometryFromWindow(const RectF &geom)
{
    if (isTransient()) {
        return;
    }
    setGeometryFromAbsolute(geom + QMarginsF(m_padding, m_padding, m_padding, m_padding));
}

void Tile::setGeometryFromAbsolute(const RectF &geom)
{
    if (isTransient()) {
        return;
    }
    const RectF outGeom = m_tiling->output()->geometryF();
    const RectF relGeom((geom.x() - outGeom.x()) / outGeom.width(),
                        (geom.y() - outGeom.y()) / outGeom.height(),
                        geom.width() / outGeom.width(),
                        geom.height() / outGeom.height());

    setRelativeGeometry(relGeom);
}

void Tile::setRelativeGeometry(const RectF &geom)
{
    if (isTransient()) {
        return;
    }
    RectF constrainedGeom = geom;
    constrainedGeom.setWidth(std::max(constrainedGeom.width(), m_minimumSize.width()));
    constrainedGeom.setHeight(std::max(constrainedGeom.height(), m_minimumSize.height()));

    if (m_relativeGeometry == constrainedGeom) {
        return;
    }

    m_relativeGeometry = constrainedGeom;

    Q_EMIT relativeGeometryChanged();
    Q_EMIT absoluteGeometryChanged();
    Q_EMIT windowGeometryChanged();

    if (isActive()) {
        for (auto *w : std::as_const(m_windows)) {
            // Resize only if we are the currently managing tile for that window
            w->moveResize(windowGeometry());
        }
    }
}

RectF Tile::relativeGeometry() const
{
    return m_relativeGeometry;
}

RectF Tile::absoluteGeometry() const
{
    const RectF geom = workspace()->clientArea(MaximizeArea, m_tiling->output());
    return RectF(std::round(geom.x() + m_relativeGeometry.x() * geom.width()),
                 std::round(geom.y() + m_relativeGeometry.y() * geom.height()),
                 std::round(m_relativeGeometry.width() * geom.width()),
                 std::round(m_relativeGeometry.height() * geom.height()));
}

RectF Tile::absoluteGeometryInScreen() const
{
    const RectF geom = workspace()->clientArea(MaximizeArea, m_tiling->output());
    return RectF(std::round(m_relativeGeometry.x() * geom.width()),
                 std::round(m_relativeGeometry.y() * geom.height()),
                 std::round(m_relativeGeometry.width() * geom.width()),
                 std::round(m_relativeGeometry.height() * geom.height()));
}

RectF Tile::windowGeometry() const
{
    // Apply half padding between tiles and full against the screen edges
    QMarginsF effectiveMargins;
    effectiveMargins.setLeft(m_relativeGeometry.left() > 0.0 ? m_padding / 2.0 : m_padding);
    effectiveMargins.setTop(m_relativeGeometry.top() > 0.0 ? m_padding / 2.0 : m_padding);
    effectiveMargins.setRight(m_relativeGeometry.right() < 1.0 ? m_padding / 2.0 : m_padding);
    effectiveMargins.setBottom(m_relativeGeometry.bottom() < 1.0 ? m_padding / 2.0 : m_padding);

    const auto geom = absoluteGeometry();
    return geom.intersected(m_tiling->output()->geometryF()) - effectiveMargins;
}

RectF Tile::maximizedWindowGeometry() const
{
    const auto geom = absoluteGeometry();
    return geom.intersected(m_tiling->output()->geometryF());
}

Qt::Edges Tile::anchors() const
{
    if (m_padding > 0) {
        return Qt::Edges();
    }
    Qt::Edges anchors = Qt::LeftEdge | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge;

    if (!qFuzzyCompare(m_relativeGeometry.left(), 0)) {
        anchors &= ~Qt::LeftEdge;
    }
    if (!qFuzzyCompare(m_relativeGeometry.top(), 0)) {
        anchors &= ~Qt::TopEdge;
    }
    if (!qFuzzyCompare(m_relativeGeometry.right(), 1)) {
        anchors &= ~Qt::RightEdge;
    }
    if (!qFuzzyCompare(m_relativeGeometry.bottom(), 1)) {
        anchors &= ~Qt::BottomEdge;
    }
    return anchors;
}

bool Tile::isRoot() const
{
    return !m_parentTile;
}

bool Tile::isLayout() const
{
    // Items with a single child are not allowed, unless the root which is *always* layout
    return m_children.count() > 0 || !m_parentTile;
}

bool Tile::canBeRemoved() const
{
    // The root tile can *never* be removed
    return !isTransient() && m_parentTile;
}

qreal Tile::padding() const
{
    // Assume padding is all the same
    return m_padding;
}

void Tile::setPadding(qreal padding)
{
    if (isTransient()) {
        return;
    }
    if (m_padding == padding) {
        return;
    }

    m_padding = padding;

    for (auto *t : std::as_const(m_children)) {
        t->setPadding(padding);
    }
    if (isActive()) {
        for (auto *w : std::as_const(m_windows)) {
            // Resize only if we are the currently managing tile for that window
            w->moveResize(windowGeometry());
        }
    }

    Q_EMIT paddingChanged(padding);
    Q_EMIT windowGeometryChanged();
}

QSizeF Tile::minimumSize() const
{
    return m_minimumSize;
}

void Tile::setMinimumSize(const QSizeF &size)
{
    if (isTransient()) {
        return;
    }
    QSizeF clampedSize = size;
    clampedSize.setWidth(std::clamp(size.width(), 0.0, 1.0));
    clampedSize.setHeight(std::clamp(size.height(), 0.0, 1.0));

    if (m_minimumSize == clampedSize) {
        return;
    }

    m_minimumSize = clampedSize;

    setRelativeGeometry(m_relativeGeometry);
    Q_EMIT minimumSizeChanged(clampedSize);
}

QuickTileMode Tile::quickTileMode() const
{
    return m_quickTileMode;
}

void Tile::setQuickTileMode(QuickTileMode mode)
{
    if (isTransient()) {
        return;
    }
    m_quickTileMode = mode;
}

Tile *Tile::rootTile() const
{
    if (!m_parentTile) {
        return const_cast<Tile *>(this);
    }
    Tile *candidate = m_parentTile;
    while (candidate->parentTile()) {
        candidate = candidate->parentTile();
    }
    return candidate;
}

void Tile::resizeFromGravity(Gravity gravity, int x_root, int y_root)
{
    if (isTransient() || !m_parentTile) {
        return;
    }

    const RectF outGeom = m_tiling->output()->geometryF();
    const QPointF relativePos = QPointF((x_root - outGeom.x()) / outGeom.width(), (y_root - outGeom.y()) / outGeom.height());
    RectF newGeom = m_relativeGeometry;

    switch (gravity) {
    case Gravity::TopLeft:
        newGeom.setTopLeft(relativePos - QPointF(m_padding / outGeom.width(), m_padding / outGeom.height()));
        break;
    case Gravity::BottomRight:
        newGeom.setBottomRight(relativePos + QPointF(m_padding / outGeom.width(), m_padding / outGeom.height()));
        break;
    case Gravity::BottomLeft:
        newGeom.setBottomLeft(relativePos + QPointF(-m_padding / outGeom.width(), m_padding / outGeom.height()));
        break;
    case Gravity::TopRight:
        newGeom.setTopRight(relativePos + QPointF(m_padding / outGeom.width(), -m_padding / outGeom.height()));
        break;
    case Gravity::Top:
        newGeom.setTop(relativePos.y() - m_padding / outGeom.height());
        break;
    case Gravity::Bottom:
        newGeom.setBottom(relativePos.y() + m_padding / outGeom.height());
        break;
    case Gravity::Left:
        newGeom.setLeft(relativePos.x() - m_padding / outGeom.width());
        break;
    case Gravity::Right:
        newGeom.setRight(relativePos.x() + m_padding / outGeom.width());
        break;
    case Gravity::Center:
        Q_UNREACHABLE();
        break;
    }

    setRelativeGeometry(newGeom);
}

void Tile::resizeByPixels(qreal delta, Qt::Edge edge)
{
    if (isTransient() || !m_parentTile) {
        return;
    }

    const auto outGeom = m_tiling->output()->geometryF();
    auto newGeom = m_relativeGeometry;

    switch (edge) {
    case Qt::LeftEdge: {
        qreal relativeDelta = delta / outGeom.width();
        newGeom.setLeft(newGeom.left() + relativeDelta);
        break;
    }
    case Qt::TopEdge: {
        qreal relativeDelta = delta / outGeom.height();
        newGeom.setTop(newGeom.top() + relativeDelta);
        break;
    }
    case Qt::RightEdge: {
        qreal relativeDelta = delta / outGeom.width();
        newGeom.setRight(newGeom.right() + relativeDelta);
        break;
    }
    case Qt::BottomEdge: {
        qreal relativeDelta = delta / outGeom.height();
        newGeom.setBottom(newGeom.bottom() + relativeDelta);
        break;
    }
    }
    setRelativeGeometry(newGeom);
}

bool Tile::manage(Window *window)
{
    if (isTransient()) {
        return false;
    }
    if (!window || window->isDeleted()) {
        // A deleted window is never reported as current successful management, not even as an
        // existing member: nothing can continue from it and nothing is stamped.
        return false;
    }
    if (!window->isResizable() || window->isAppletPopup()) {
        return false;
    }

    if (m_windows.contains(window)) {
        // An otherwise-valid public no-op is newer same-window intent for any older operation.
        window->noteTileMembershipChanged();
        return true;
    }

    // If the window is not on the same tile's desktop
    // it can't be added
    if (!window->isOnDesktop(m_desktop)) {
        return false;
    }

    // The public entry point establishes its own operation after the mutation-free rejections: the
    // qualified cancellation is its single stamp (a valid manual intent revokes automatic ownership
    // before any new membership is published), a current continuation enters the guarded step, a
    // refusal without effect selects the legacy remainder, and a supersession stops here with false.
    // The destination's identity is held across the cancellation's callbacks; nothing is entered
    // after it disappeared.
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Tile> self(this);
    const auto context = assignmentContext(window);
    auto preparation = window->cancelTransientRequestForAssignment();
    if (preparation.outcome == Window::AssignmentStepOutcome::Superseded || !self) {
        return false;
    }
    if (preparation.outcome == Window::AssignmentStepOutcome::AppliedCurrent) {
        if (!context()) {
            // The cancellation's callbacks changed the destination's context: the operation does
            // not capture that changed state as fresh.
            return false;
        }
        const auto step = manageForAssignment(window, *preparation.checkpoint);
        if (step.outcome == Window::AssignmentStepOutcome::AppliedCurrent) {
            return true;
        }
        if (step.outcome == Window::AssignmentStepOutcome::Superseded) {
            return false;
        }
        return manageLegacy(window);
    }
    // Refused for exhaustion before the stamp: the legacy cancellation keeps today's behaviour at
    // that edge. This is a legacy exclusion, not a guarded continuation.
    window->cancelTransientRequest();
    return self && manageLegacy(window);
}

bool Tile::manageLegacy(Window *window)
{
    // The cancellation's callbacks ran before this remainder: the same-window no-op rule applies
    // at its entry, and the desktop check is the legacy body's own.
    if (m_windows.contains(window)) {
        window->noteTileMembershipChanged();
        return true;
    }
    if (!window->isOnDesktop(m_desktop)) {
        return false;
    }

    // There can be only one window owner per root tile, so make it forget
    // if anybody else had the association
    bool evacuated = false;
    const auto evacuate = [&evacuated, window](Tile *tile) {
        if (tile->remove(window)) {
            evacuated = true;
        }
    };

    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        if (TileManager *manager = workspace()->tileManager(output)) {
            if (Tile *rootTile = manager->rootTile(m_desktop)) {
                rootTile->visitDescendants(evacuate);
            }
            if (Tile *rootTile = manager->quickRootTile(m_desktop)) {
                rootTile->visitDescendants(evacuate);
            }
        }
    }

    add(window);

    // We can actually manage the window geometry if our desktop is current
    // or if our desktop is the only desktop the window is in
    if (isActive()) {
        window->requestTile(this);
    } else if (evacuated) {
        window->requestTile(nullptr);
    }

    return true;
}

bool Tile::releaseTransientWindow(Window *)
{
    return false;
}

bool Tile::unmanage(Window *window)
{
    if (isTransient()) {
        return releaseTransientWindow(window);
    }
    if (!window || !m_windows.contains(window)) {
        // The legacy miss: a membership-only stamp, never a cancellation; a null argument stays a
        // plain miss.
        if (window) {
            window->noteTileMembershipChanged();
        }
        return false;
    }
    if (window->isDeleted()) {
        // Nothing can continue from a deleted window (its qualified cancellation is stale by
        // design), but a dead member must still be released: the legacy membership-only removal,
        // on which the workspace's removal notification relies. No cancellation, no guarded step.
        return unmanageLegacy(window);
    }
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Tile> self(this);
    auto preparation = window->cancelTransientRequestForAssignment();
    if (preparation.outcome == Window::AssignmentStepOutcome::Superseded || !self) {
        return false;
    }
    if (preparation.outcome == Window::AssignmentStepOutcome::AppliedCurrent) {
        if (!m_windows.contains(window)) {
            // The cancellation's callbacks removed the membership: newer intent, nothing to do here.
            return false;
        }
        const auto step = unmanageForAssignment(window, *preparation.checkpoint);
        if (step.outcome == Window::AssignmentStepOutcome::AppliedCurrent) {
            return true;
        }
        if (step.outcome == Window::AssignmentStepOutcome::Superseded) {
            return false;
        }
        return unmanageLegacy(window);
    }
    window->cancelTransientRequest();
    return self && unmanageLegacy(window);
}

bool Tile::unmanageLegacy(Window *window)
{
    if (!remove(window)) {
        // Nothing to remove: still newer same-window intent for any older operation (conservative
        // rule); a transient target above keeps its refusal contract instead. A null argument
        // (reachable from scripting) stays a plain miss.
        if (window) {
            window->noteTileMembershipChanged();
        }
        return false;
    }

    if (window->requestedTile() == this) {
        window->requestTile(nullptr);
    }
    return true;
}

void Tile::forget(Window *window)
{
    if (isTransient()) {
        releaseTransientWindow(window);
        return;
    }
    if (!window || !m_windows.contains(window)) {
        if (window) {
            window->noteTileMembershipChanged();
        }
        return;
    }
    if (window->isDeleted()) {
        forgetLegacy(window);
        return;
    }
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Tile> self(this);
    auto preparation = window->cancelTransientRequestForAssignment();
    if (preparation.outcome == Window::AssignmentStepOutcome::Superseded || !self) {
        return;
    }
    if (preparation.outcome == Window::AssignmentStepOutcome::AppliedCurrent) {
        if (!m_windows.contains(window)) {
            return;
        }
        if (forgetForAssignment(window, *preparation.checkpoint).outcome != Window::AssignmentStepOutcome::Refused) {
            return;
        }
        forgetLegacy(window);
        return;
    }
    window->cancelTransientRequest();
    if (self) {
        forgetLegacy(window);
    }
}

void Tile::forgetLegacy(Window *window)
{
    if (remove(window)) {
        window->forgetTile(this);
    } else if (window) {
        window->noteTileMembershipChanged();
    }
}

void Tile::add(Window *window)
{
    Q_ASSERT(!m_windows.contains(window));
    m_windows.append(window);
    // Recorded before the notification, so a checkpoint taken by a guarded caller after the write
    // and before the callbacks sees this change as its own and any later one as foreign.
    window->noteTileMembershipChanged();

    Q_EMIT windowAdded(window);
    Q_EMIT windowsChanged();
}

bool Tile::remove(Window *window)
{
    if (!m_windows.removeOne(window)) {
        // An internal miss (an evacuation that found nothing) records nothing.
        return false;
    }
    window->noteTileMembershipChanged();

    Q_EMIT windowRemoved(window);
    Q_EMIT windowsChanged();
    return true;
}

QList<KWin::Window *> Tile::windows() const
{
    return m_windows;
}

bool Tile::destinationIsLive() const
{
    // The teardown flag is set only by the manager's destructor: a live destination is validated
    // against the workspace's current outputs and desktops.
    return m_tiling && !m_tiling->tearingDown() && m_tiling->output() && workspace()->outputs().contains(m_tiling->output())
        && m_desktop && VirtualDesktopManager::self()->desktops().contains(m_desktop);
}

Window::AssignmentStepResult Tile::membershipNoOp(Window *window, Window::OwnedProvenance owned) const
{
    // An operation-owned no-op: nothing written, no stamp (the public entry stamps), a fresh
    // checkpoint, the incoming provenance preserved.
    Window::AssignmentStepResult result;
    result.owned = std::move(owned);
    result.checkpoint = window->makeTileAssignmentCheckpoint();
    if (window->assignmentResultIsCurrent(result)) {
        result.outcome = Window::AssignmentStepOutcome::AppliedCurrent;
    } else {
        result.outcome = Window::AssignmentStepOutcome::Superseded;
        result.checkpoint.reset();
    }
    return result;
}

std::function<bool()> Tile::assignmentContext(Window *window) const
{
    const QPointer<const Tile> self(this);
    const QPointer<TileManager> manager(m_tiling);
    const QPointer<VirtualDesktop> desktop(m_desktop);
    const QPointer<Tile> parent(m_parentTile);
    const bool hadParent = m_parentTile;
    const bool active = isActive();
    const RectF target = windowGeometry();
    const QPointer<Window> guarded(window);
    const QList<VirtualDesktop *> desktops = window->desktops();
    return [self, manager, desktop, parent, hadParent, active, target, guarded, desktops]() {
        return self && manager && desktop && guarded && !guarded->isDeleted() && self->m_tiling == manager && self->m_desktop == desktop
            && self->destinationIsLive() && (!hadParent || parent) && self->m_parentTile == parent.data() && self->isActive() == active
            && self->windowGeometry() == target && guarded->desktops() == desktops;
    };
}

void Tile::removeForAssignment(Window *window, std::unique_ptr<TileAssignmentFence> &checkpoint, const std::function<bool()> &current)
{
    // The guarded removal: the write and its revision, the checkpoint sealing them, then the two
    // notifications as the write's obligation, the second only while the operation's complete
    // currentness (the caller's context, provenance and the checkpoint just taken) still holds.
    m_windows.removeOne(window);
    window->noteTileMembershipChanged();
    checkpoint = window->makeTileAssignmentCheckpoint();
    const QPointer<Tile> self(this);
    Q_EMIT windowRemoved(window);
    if (self && current()) {
        Q_EMIT windowsChanged();
    }
}

Window::AssignmentStepResult Tile::manageForAssignment(Window *window, const TileAssignmentFence &before, Window::OwnedProvenance owned)
{
    using Outcome = Window::AssignmentStepOutcome;
    Window::AssignmentStepResult result;
    if (!window) {
        return result;
    }
    if (!before.isCurrentFor(*window) || !window->ownedProvenanceIsCurrent(owned)) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    // Mutation-free rejections, the backend first: a backend whose protocol state commits
    // immediately has no guarded membership step at all.
    if (isTransient() || !window->supportsAssignmentGeometryRequest() || !window->isResizable() || window->isAppletPopup()
        || !window->isOnDesktop(m_desktop) || !destinationIsLive()) {
        return result;
    }
    if (m_windows.contains(window)) {
        return membershipNoOp(window, std::move(owned));
    }
    // The worklist, collected without callbacks: every tile of this desktop across outputs that
    // holds the window, with its manager and desktop captured. Source outputs need not stay
    // registered; the destination must.
    struct Source
    {
        QPointer<Tile> tile;
        QPointer<TileManager> manager;
        QPointer<VirtualDesktop> desktop;
    };
    QList<Source> sources;
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        TileManager *manager = workspace()->tileManager(output);
        if (!manager) {
            continue;
        }
        for (Tile *root : {static_cast<Tile *>(manager->rootTile(m_desktop)), static_cast<Tile *>(manager->quickRootTile(m_desktop))}) {
            if (root) {
                root->visitDescendants([&sources, window](Tile *tile) {
                    if (tile->m_windows.contains(window)) {
                        sources.append(Source{tile, tile->m_tiling, tile->m_desktop});
                    }
                });
            }
        }
    }
    const bool active = isActive();
    const bool evacuates = !sources.isEmpty();
    // The follow-up's read-only preflight before any mutation: the association when this desktop
    // is active, the null association when an inactive one evacuates; and this step's own
    // membership headroom, one increment per removal and one for the addition.
    if ((active && !window->canRequestTileForAssignment(this)) || (!active && evacuates && !window->canRequestTileForAssignment(nullptr))) {
        return result;
    }
    const quint64 increments = quint64(sources.size()) + 1;
    if (window->m_tileMembershipChange >= std::numeric_limits<quint64>::max() - increments) {
        return result;
    }
    // The frozen context as a predicate (the Window retained): composed into every continuation
    // of this step together with the incoming provenance and the current checkpoint, and chained
    // into the provenance the association step receives and returns, so its own callbacks are held
    // to the same context.
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Tile> self(this);
    const auto context = assignmentContext(window);
    const Window::OwnedProvenance chained = owned.withConstraint(context);
    std::unique_ptr<TileAssignmentFence> checkpoint = window->makeTileAssignmentCheckpoint();
    const auto current = [&]() {
        return window->ownedProvenanceIsCurrent(chained) && checkpoint && checkpoint->isCurrentFor(*window);
    };
    for (const Source &source : std::as_const(sources)) {
        if (!current()) {
            result.outcome = Outcome::Superseded;
            return result;
        }
        if (!source.tile || !source.tile->m_windows.contains(window)) {
            // Gone or emptied while the checkpoint still validates: this window's membership did
            // not change there (a destroyed source that held it would have changed it).
            continue;
        }
        if (source.tile->m_tiling != source.manager || source.tile->m_desktop != source.desktop) {
            // A source moved to another manager or desktop is foreign context, not a miss.
            result.outcome = Outcome::Superseded;
            return result;
        }
        source.tile->removeForAssignment(window, checkpoint, current);
    }
    if (!current()) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    // The addition, sealed by its own checkpoint, its notifications as its obligation (the second
    // only while the complete currentness holds).
    m_windows.append(window);
    window->noteTileMembershipChanged();
    checkpoint = window->makeTileAssignmentCheckpoint();
    Q_EMIT windowAdded(window);
    if (self && current()) {
        Q_EMIT windowsChanged();
    }
    if (!current()) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    if (active || evacuates) {
        // The association step chaining the last checkpoint under the chained provenance; its
        // refusal after this step's mutations is this step's supersession, never a fallback.
        auto step = window->requestTileForAssignment(active ? this : nullptr, *checkpoint, chained);
        if (step.outcome != Outcome::AppliedCurrent) {
            result.outcome = Outcome::Superseded;
            return result;
        }
        return step;
    }
    result.owned = chained;
    result.checkpoint = std::move(checkpoint);
    if (window->assignmentResultIsCurrent(result)) {
        result.outcome = Outcome::AppliedCurrent;
    } else {
        result.outcome = Outcome::Superseded;
        result.checkpoint.reset();
    }
    return result;
}

Window::AssignmentStepResult Tile::unmanageForAssignment(Window *window, const TileAssignmentFence &before, Window::OwnedProvenance owned)
{
    using Outcome = Window::AssignmentStepOutcome;
    Window::AssignmentStepResult result;
    if (!window) {
        return result;
    }
    if (!before.isCurrentFor(*window) || !window->ownedProvenanceIsCurrent(owned)) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    // A source need not be on a registered output; it must not be tearing down.
    if (isTransient() || !window->supportsAssignmentGeometryRequest() || !m_tiling || m_tiling->tearingDown()) {
        return result;
    }
    if (!m_windows.contains(window)) {
        return membershipNoOp(window, std::move(owned));
    }
    // The follow-up preflighted before the removal: the null association when this is the
    // requested tile; and one membership increment.
    const bool clears = window->requestedTile() == this;
    if ((clears && !window->canRequestTileForAssignment(nullptr))
        || window->m_tileMembershipChange >= std::numeric_limits<quint64>::max() - 1) {
        return result;
    }
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Tile> self(this);
    std::unique_ptr<TileAssignmentFence> checkpoint = window->makeTileAssignmentCheckpoint();
    const auto current = [&]() {
        return self && !window->isDeleted() && window->ownedProvenanceIsCurrent(owned) && checkpoint && checkpoint->isCurrentFor(*window);
    };
    removeForAssignment(window, checkpoint, current);
    if (!current()) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    if (clears) {
        auto step = window->requestTileForAssignment(nullptr, *checkpoint, owned);
        if (step.outcome != Outcome::AppliedCurrent) {
            result.outcome = Outcome::Superseded;
            return result;
        }
        return step;
    }
    result.owned = std::move(owned);
    result.checkpoint = std::move(checkpoint);
    if (window->assignmentResultIsCurrent(result)) {
        result.outcome = Outcome::AppliedCurrent;
    } else {
        result.outcome = Outcome::Superseded;
        result.checkpoint.reset();
    }
    return result;
}

Window::AssignmentStepResult Tile::forgetForAssignment(Window *window, const TileAssignmentFence &before, Window::OwnedProvenance owned)
{
    using Outcome = Window::AssignmentStepOutcome;
    Window::AssignmentStepResult result;
    if (!window) {
        return result;
    }
    if (!before.isCurrentFor(*window) || !window->ownedProvenanceIsCurrent(owned)) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    if (isTransient() || !window->supportsAssignmentGeometryRequest() || !m_tiling || m_tiling->tearingDown()) {
        return result;
    }
    if (!m_windows.contains(window)) {
        return membershipNoOp(window, std::move(owned));
    }
    // The forget step's own refusals, preflighted before the removal; and one membership increment.
    if (!window->canForgetTileForAssignment() || window->m_tileMembershipChange >= std::numeric_limits<quint64>::max() - 1) {
        return result;
    }
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    const QPointer<Tile> self(this);
    std::unique_ptr<TileAssignmentFence> checkpoint = window->makeTileAssignmentCheckpoint();
    const auto current = [&]() {
        return self && !window->isDeleted() && window->ownedProvenanceIsCurrent(owned) && checkpoint && checkpoint->isCurrentFor(*window);
    };
    removeForAssignment(window, checkpoint, current);
    if (!current()) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    auto step = window->forgetTileForAssignment(this, *checkpoint, owned);
    if (step.outcome != Outcome::AppliedCurrent) {
        result.outcome = Outcome::Superseded;
        return result;
    }
    return step;
}

void Tile::insertChild(int position, Tile *item)
{
    Q_ASSERT(position >= 0);
    const bool wasEmpty = m_children.isEmpty();
    item->setParent(this);

    m_children.insert(std::clamp<qsizetype>(position, 0, m_children.length()), item);

    if (wasEmpty) {
        Q_EMIT isLayoutChanged(true);
        auto windows = m_windows;
        for (auto *w : windows) {
            Tile *tile = m_tiling->rootTile(m_desktop)->pick(w->moveResizeGeometry().center());
            unmanage(w);
            if (tile) {
                tile->manage(w);
            }
        }
    }

    Q_EMIT childTilesChanged();
}

void Tile::destroyChild(Tile *tile)
{
    if (isTransient() || (tile && tile->isTransient())) {
        return;
    }
    removeChild(tile);
    delete tile;
}

void Tile::removeChild(Tile *child)
{
    const bool wasEmpty = m_children.isEmpty();
    const int idx = m_children.indexOf(child);
    m_children.removeAll(child);
    if (m_children.isEmpty() && !wasEmpty) {
        Q_EMIT isLayoutChanged(false);
    }
    if (idx > -1) {
        for (int i = idx; i < m_children.count(); ++i) {
            Q_EMIT m_children[i]->rowChanged(i);
        }
    }
    Q_EMIT childTilesChanged();
}

QList<Tile *> Tile::childTiles() const
{
    return m_children;
}

Tile *Tile::childTile(int row)
{
    if (row < 0 || row >= m_children.size()) {
        return nullptr;
    }
    return m_children.value(row);
}

int Tile::childCount() const
{
    return m_children.count();
}

QList<Tile *> Tile::descendants() const
{
    QList<Tile *> tiles;
    for (auto *t : std::as_const(m_children)) {
        tiles << t << t->descendants();
    }
    return tiles;
}

Tile *Tile::parentTile() const
{
    return m_parentTile;
}

void Tile::visitDescendants(std::function<void(Tile *child)> callback)
{
    callback(this);
    for (Tile *child : m_children) {
        child->visitDescendants(callback);
    }
}

TileManager *Tile::manager() const
{
    return m_tiling;
}

int Tile::row() const
{
    if (m_parentTile) {
        return m_parentTile->m_children.indexOf(this);
    }

    return -1;
}

Tile *Tile::nextSibling() const
{
    const int r = row();
    if (!m_parentTile || row() >= m_parentTile->childCount() - 1) {
        return nullptr;
    } else {
        return m_parentTile->childTiles()[r + 1];
    }
}

Tile *Tile::previousSibling() const
{
    const int r = row();
    if (r <= 0 || !m_parentTile) {
        return nullptr;
    } else {
        return m_parentTile->childTiles().at(r - 1);
    }
}

} // namespace KWin

#include "moc_tile.cpp"
