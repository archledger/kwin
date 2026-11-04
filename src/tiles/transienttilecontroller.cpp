/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "transienttilecontroller.h"

#include "config-kwin.h"
#include "tile.h"
#include "tilemanager.h"
#include "transientplacementwriter.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#if KWIN_BUILD_ACTIVITIES
#include "activities.h"
#endif

#include <QScopeGuard>

#include <cmath>
#include <limits>

namespace KWin
{

static bool validContext(TileManager *manager, VirtualDesktop *desktop)
{
    return manager && desktop && !manager->tearingDown() && manager->output()
        && VirtualDesktopManager::self()->desktops().contains(desktop) && manager->rootTile(desktop);
}

class TransientTileController::Mutation
{
public:
    explicit Mutation(TransientTileController *controller)
        : m_controller(controller)
        , m_wasMutating(controller->m_mutating)
    {
        controller->m_mutating = true;
        captureTree();
    }

    ~Mutation()
    {
        // A signal may delete the controller while its operation is still on the stack.
        if (m_controller) {
            m_controller->m_mutating = m_wasMutating;
        }
    }

    void captureTree()
    {
        m_generation = m_controller->m_generation;
        m_root = m_controller->m_root;
        m_tile = m_controller->m_tile;
        m_manager = m_controller->m_manager;
        m_desktop = m_controller->m_desktop;
    }

    bool current(const TransientTile *tile) const;

private:
    Q_DISABLE_COPY_MOVE(Mutation)
    QPointer<TransientTileController> m_controller;
    QPointer<TransientTile> m_root;
    QPointer<TransientTile> m_tile;
    QPointer<TileManager> m_manager;
    QPointer<VirtualDesktop> m_desktop;
    quint64 m_generation = 0;
    bool m_wasMutating;
};

class TransientTile final : public Tile
{
public:
    TransientTile(TransientTileController *controller, TileManager *manager, VirtualDesktop *desktop, const RectF &geometry)
        : Tile(manager, nullptr, Type::Transient)
        , m_controller(controller)
    {
        m_desktop = desktop;
        m_relativeGeometry = geometry;
    }

    ~TransientTile() override
    {
        if (m_registration) {
            m_registration->destroying = true;
        }
        m_destroying = true;
        if (m_controller) {
            m_controller->nodeAboutToBeDestroyed(this);
        }
    }

private:
    friend class TransientTileController;

    bool releaseTransientWindow(Window *window) override
    {
        const auto controller = m_controller;
        if (!window || m_destroying || !controller || controller->m_shuttingDown
            || controller->m_tile != this || controller->m_window != window
            || !controller->m_membershipPublished || !m_windows.contains(window) || !controller->m_ownershipGeneration) {
            return false;
        }
        const QPointer<TransientTile> guard(this);
        const auto generation = controller->m_ownershipGeneration;
        controller->releaseWindow(window, generation);
        // Release reports removal even when native restoration or close cleanup is pending.
        return !guard || !guard->m_windows.contains(window);
    }

    bool addMember(Window *window)
    {
        if (m_destroying || !m_windows.isEmpty()) {
            return false;
        }
        m_windows.append(window);
        return true;
    }

    void removeMember(Window *window)
    {
        m_windows.removeAll(window);
    }

    // The membership predicate is the admission's own currentness over the window's membership
    // revision, checked after each callback boundary so a superseded admission publishes nothing
    // further; the tile itself cannot read that revision.
    bool publishAdded(Window *window, quint64 generation, const std::function<bool()> &membershipCurrent)
    {
        const QPointer<TransientTile> guard(this);
        const auto controller = m_controller;
        Q_EMIT windowAdded(window);
        if (!guard || !controller || !controller->admissionCurrent(window, generation) || !membershipCurrent()) {
            return false;
        }
        Q_EMIT windowsChanged();
        return guard && controller && controller->admissionCurrent(window, generation) && membershipCurrent();
    }

    void publishRemoved(Window *window, quint64 generation)
    {
        const QPointer<TransientTile> guard(this);
        const auto controller = m_controller;
        Q_EMIT windowRemoved(window);
        if (guard && !guard->m_destroying && controller && !controller->m_shuttingDown
            && controller->m_generation == generation && controller->m_tile == guard) {
            Q_EMIT windowsChanged();
        }
    }

    void linkToRoot(TransientTile *root)
    {
        // Establish both links privately without Tile::insertChild's manual evacuation.
        m_parentTile = root;
        root->m_children.append(this);
        setParent(root);
    }

    void detachForDestruction()
    {
        if (m_parentTile) {
            auto root = static_cast<TransientTile *>(m_parentTile);
            root->m_children.removeAll(this);
            m_parentTile = nullptr;
        }
        // ChildRemoved observers must not still own the leaf that is being destroyed.
        setParent(nullptr);
    }

    bool applyGeometry(const RectF &geometry, const TransientTileController::Mutation &mutation)
    {
        if (!mutation.current(this)) {
            return false;
        }
        if (m_relativeGeometry.x() == geometry.x() && m_relativeGeometry.y() == geometry.y()
            && m_relativeGeometry.width() == geometry.width() && m_relativeGeometry.height() == geometry.height()) {
            return true;
        }
        m_relativeGeometry = geometry;
        Q_EMIT relativeGeometryChanged();
        if (!mutation.current(this)) {
            return false;
        }
        Q_EMIT absoluteGeometryChanged();
        if (!mutation.current(this)) {
            return false;
        }
        Q_EMIT windowGeometryChanged();
        return mutation.current(this);
    }

    QPointer<TransientTileController> m_controller;
    std::shared_ptr<TileManager::TransientRegistration> m_registration;
    bool m_destroying = false;
};

class TransientTileController::Release
{
public:
    explicit Release(TransientTileController *controller, bool notifyLeaf = true, Window::AssignmentStepResult *guarded = nullptr)
        : m_controller(controller)
        , m_window(controller->m_window)
        , m_tile(controller->m_tile)
        , m_generation(controller->m_ownershipGeneration)
        , m_treeGeneration(controller->m_generation)
        , m_notifyLeaf(notifyLeaf)
        , m_guarded(guarded)
    {
        if (!m_window || !m_generation) {
            m_window.clear();
            return;
        }
        m_window->ref();
        m_active = m_window->beginTransientRelease(m_generation);
        if (!m_active) {
            return;
        }
        // All state changes before finish() are callback-free.
        controller->clearAdmission();
        m_window->m_transientOwnership->controller.clear();
        if (m_tile && m_tile->m_windows.contains(m_window)) {
            m_tile->removeMember(m_window);
            m_window->noteTileMembershipChanged();
        }
        m_changes = m_window->detachTransientAssociation(m_generation);
        if (m_guarded) {
            // After this release's own callback-free writes, before its notifications; never
            // refreshed. The result pointer is stack-bounded by the releasing call.
            m_guarded->checkpoint = m_window->makeTileAssignmentCheckpoint();
        }
    }

    ~Release()
    {
        finish();
        if (m_window) {
            m_window->unref();
        }
    }

    bool finish()
    {
        if (m_finished) {
            return m_finalized;
        }
        m_finished = true;
        if (!m_active || !m_window) {
            return false;
        }
        m_window->notifyTransientAssociation(m_generation, m_changes);
        if (m_notifyLeaf && m_tile && !m_tile->m_destroying && m_controller && !m_controller->m_shuttingDown
            && m_controller->m_generation == m_treeGeneration && m_controller->m_tile == m_tile) {
            m_tile->publishRemoved(m_window, m_treeGeneration);
        }
        m_finalized = m_window->finishTransientRelease(m_generation);
        if (m_guarded) {
            // The caller's continuation, decided once after every notification and the
            // finalization, before the releasing call returns.
            m_window->qualifyReleaseContinuation(*m_guarded);
        }
        return m_finalized;
    }

private:
    Q_DISABLE_COPY_MOVE(Release)
    QPointer<TransientTileController> m_controller;
    QPointer<Window> m_window;
    QPointer<TransientTile> m_tile;
    Window::TransientAssociationChanges m_changes;
    quint64 m_generation;
    quint64 m_treeGeneration;
    bool m_notifyLeaf;
    Window::AssignmentStepResult *m_guarded = nullptr;
    bool m_active = false;
    bool m_finished = false;
    bool m_finalized = false;
};

class TransientTileController::Request final : public Window::TransientTileRequest
{
public:
    Request(TransientTileController *controller, Window *window, quint64 generation)
        : TransientTileRequest(window, controller->m_tile, 0)
        , m_controller(controller)
        , m_window(window)
        , m_generation(generation)
    {
    }

private:
    bool authorityIsCurrent() const override
    {
        return m_controller && m_controller->admissionCurrent(m_window, m_generation);
    }

    QPointer<TransientTileController> m_controller;
    QPointer<Window> m_window;
    quint64 m_generation;
};

static QString placementKey(Window *window)
{
    QString key = window->desktopFileName();
    if (key.isEmpty()) {
        key = window->resourceClass();
    } else {
        const auto slash = key.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0) {
            key = key.mid(slash + 1);
        }
        if (key.endsWith(QStringLiteral(".desktop"))) {
            key.chop(8);
        }
    }
    return key.toLower();
}

static bool sameFrame(const RectF &left, const RectF &right)
{
    return left.x() == right.x() && left.y() == right.y() && left.width() == right.width() && left.height() == right.height();
}

static std::optional<QString> activityContext(Window *window)
{
#if KWIN_BUILD_ACTIVITIES
    if (auto activities = workspace()->activities()) {
        if (activities->serviceStatus() != KActivities::Consumer::Running) {
            return std::nullopt;
        }
        const auto all = activities->all();
        const auto current = activities->current();
        const auto selected = window->activities();
        if (current.isEmpty() || !all.contains(current)
            || (selected.isEmpty() ? all.size() != 1 : selected.size() != 1 || selected.constFirst() != current)) {
            return std::nullopt;
        }
        return current;
    }
#endif
    return QString();
}

static bool freeOfNativeMembership(Window *window)
{
    for (auto output : workspace()->outputs()) {
        auto manager = workspace()->tileManager(output);
        if (!manager || manager->tearingDown()) {
            return false;
        }
        for (auto desktop : VirtualDesktopManager::self()->desktops()) {
            if (!manager->rootTile(desktop) || !manager->quickRootTile(desktop) || manager->tileForWindow(window, desktop)) {
                return false;
            }
        }
    }
    return true;
}

bool TransientTileController::Mutation::current(const TransientTile *tile) const
{
    return m_controller && !m_controller->m_shuttingDown && m_generation == m_controller->m_generation
        && m_root && m_tile && m_tile == tile && m_manager && m_desktop
        && m_controller->m_root == m_root && m_controller->m_tile == m_tile
        && m_controller->m_manager == m_manager && m_controller->m_desktop == m_desktop
        && validContext(m_manager, m_desktop);
}

static bool validGeometry(const RectF &geometry)
{
    for (qreal value : {geometry.x(), geometry.y(), geometry.width(), geometry.height()}) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return geometry.x() >= 0 && geometry.y() >= 0 && geometry.width() > 0 && geometry.height() > 0
        && geometry.x() + geometry.width() <= 1 && geometry.y() + geometry.height() <= 1;
}

TransientTileController::TransientTileController(QObject *parent)
    : QObject(parent)
{
    connect(VirtualDesktopManager::self(), &VirtualDesktopManager::desktopRemoved, this, [this](VirtualDesktop *desktop) {
        if (m_desktop == desktop) {
            clearTree(m_generation);
        }
    });
}

TransientTileController::~TransientTileController()
{
    m_shuttingDown = true;
    Release release(this, false);
    const auto root = m_root;
    resetTree();
    if (root) {
        root->setParent(nullptr);
    }
    delete root.data();
    release.finish();
}

TransientTileController::Result TransientTileController::createTree(TileManager *manager, VirtualDesktop *desktop, const RectF &geometry)
{
    if (m_mutating || m_shuttingDown || m_root || m_tile) {
        return Result::Busy;
    }
    if (!validContext(manager, desktop)) {
        return Result::InvalidContext;
    }
    if (!manager->canRegisterTransientRoot(desktop)) {
        return Result::Busy;
    }
    if (!validGeometry(geometry)) {
        return Result::InvalidGeometry;
    }
    if (m_generation == std::numeric_limits<quint64>::max()) {
        return Result::GenerationExhausted;
    }
    Mutation mutation(this);
    QPointer<TransientTileController> guard(this);
    QPointer<TileManager> contextManager(manager);
    QPointer<VirtualDesktop> contextDesktop(desktop);
    QPointer<TransientTile> root;
    QPointer<TransientTile> tile;
    auto cleanup = qScopeGuard([&]() {
        if (root) {
            // A destroyed callback may delete the controller while this root is unpublished.
            root->setParent(nullptr);
        }
        delete root.data();
        delete tile.data();
    });
    root = new TransientTile(this, contextManager, contextDesktop, RectF(0, 0, 1, 1));
    tile = new TransientTile(this, contextManager, contextDesktop, geometry);
    const auto contextValid = [&]() {
        return guard && !guard->m_shuttingDown && validContext(contextManager, contextDesktop);
    };
    tile->linkToRoot(root);
    if (!contextValid() || !root || !tile) {
        return Result::Superseded;
    }
    root->setParent(this);
    if (!contextValid() || !root || !tile) {
        return Result::Superseded;
    }
    m_root = root;
    m_tile = tile;
    m_manager = contextManager;
    m_desktop = contextDesktop;
    ++m_generation;
    root->m_registration = contextManager->registerTransientRoot(root, this, m_generation);
    if (!root->m_registration) {
        resetTree();
        return Result::Refused;
    }
    m_managerDestroyed = connect(contextManager, &QObject::destroyed, this, [this]() {
        clearTree(m_generation);
    });
    m_desktopDestroyed = connect(contextDesktop, &QObject::destroyed, this, [this]() {
        clearTree(m_generation);
    });
    mutation.captureTree();
    cleanup.dismiss();
    Q_EMIT treeChanged();
    return mutation.current(tile) ? Result::Accepted : Result::Superseded;
}

TransientTileController::Result TransientTileController::setGeometry(quint64 generation, const RectF &geometry)
{
    if (m_mutating || m_shuttingDown || m_window) {
        return Result::Busy;
    }
    if (!m_root || !m_tile || generation == 0 || generation != m_generation) {
        return Result::StaleGeneration;
    }
    if (!validContext(m_manager, m_desktop)) {
        return Result::InvalidContext;
    }
    if (!validGeometry(geometry)) {
        return Result::InvalidGeometry;
    }
    Mutation mutation(this);
    return m_tile->applyGeometry(geometry, mutation) ? Result::Accepted : Result::Superseded;
}

TransientTileController::Result TransientTileController::clearTree(quint64 generation)
{
    if (m_shuttingDown) {
        return Result::Busy;
    }
    if (!m_root || generation == 0 || generation != m_generation) {
        return Result::StaleGeneration;
    }
    Mutation mutation(this);
    Release release(this, false);
    const auto root = m_root;
    resetTree();
    QPointer<TransientTileController> guard(this);
    root->setParent(nullptr);
    delete root.data();
    release.finish();
    if (!guard) {
        return Result::Superseded;
    }
    Q_EMIT treeChanged();
    return guard ? Result::Accepted : Result::Superseded;
}

void TransientTileController::resetTree()
{
    if (m_manager && m_root) {
        m_manager->unregisterTransientRoot(m_desktop, m_root, this, m_generation);
    }
    disconnect(m_managerDestroyed);
    disconnect(m_desktopDestroyed);
    m_root.clear();
    m_tile.clear();
    m_manager.clear();
    m_desktop.clear();
}

void TransientTileController::nodeAboutToBeDestroyed(TransientTile *node)
{
    if (node != m_root && node != m_tile) {
        return;
    }
    Mutation mutation(this);
    Release release(this, false);
    const auto root = m_root;
    const auto generation = m_generation;
    resetTree();
    QPointer<TransientTileController> guard(this);
    node->detachForDestruction();
    if (root) {
        root->setParent(nullptr);
    }
    if (root && root != node) {
        // Do not delete the parent while its child is still unwinding its destructor.
        root->deleteLater();
    }
    release.finish();
    if (guard) {
        QMetaObject::invokeMethod(this, [this, generation]() {
            if (!m_shuttingDown && m_generation == generation) {
                Q_EMIT treeChanged();
            }
        }, Qt::QueuedConnection);
    }
}

void TransientTileController::clearAdmission()
{
    disconnect(m_windowClosed);
    disconnect(m_providerRetired);
    disconnect(m_keyConflict);
    m_window.clear();
    m_ownershipGeneration = 0;
    m_membershipPublished = false;
    m_key.clear();
    m_activity.clear();
}

bool TransientTileController::admissionCurrent(Window *window, quint64 generation) const
{
    if (m_shuttingDown || !m_root || !m_tile || generation != m_generation || m_window != window
        || !window || window->isDeleted() || !window->isNormalWindow() || window->isTransient() || window->isMinimized()
        || !validContext(m_manager, m_desktop) || window->moveResizeOutput() != m_manager->output()
        || window->desktops().size() != 1 || window->desktops().constFirst() != m_desktop || placementKey(window) != m_key) {
        return false;
    }
    const auto activity = activityContext(window);
    if (!activity || *activity != m_activity) {
        return false;
    }
    if (m_ownershipGeneration && (!window->m_transientOwnership || window->m_transientOwnership->generation != m_ownershipGeneration || window->m_transientOwnership->controller != this)) {
        return false;
    }
    if (m_ownershipGeneration) {
        const auto &ownership = *window->m_transientOwnership;
        if (!ownership.writer || !ownership.writer->isProviderCurrent(ownership.provider, ownership.providerRevision)
            || !sameFrame(ownership.target, m_tile->windowGeometry()) || !window->isTransientTileTargetFeasible(ownership.target)) {
            return false;
        }
        if (ownership.progress == Window::TransientRequestProgress::NoRequest
            && (window->geometryRequestGeneration() != ownership.initialGeometryGeneration
                || !sameFrame(window->frameGeometry(), ownership.floating) || !sameFrame(window->moveResizeGeometry(), ownership.floating)
                || (!ownership.executing && !window->canStartTransientTileRequest(ownership.target)))) {
            return false;
        }
    }
    return !m_membershipPublished
        || (m_tile->windows().contains(window) && window->m_transientOwnership
            && window->m_transientOwnership->phase == Window::TransientOwnershipPhase::Owned
            && window->m_transientOwnership->writer
            && window->m_transientOwnership->writer->ownsReservation(window->internalId(), QString::number(m_ownershipGeneration)));
}

TransientTileController::Result TransientTileController::admit(quint64 generation, Window *window, TransientPlacementWriter *writer,
                                                               const QUuid &provider, const QString &key, quint64 providerRevision, quint64 mapRevision)
{
    if (m_mutating || m_shuttingDown || m_window || (window && window->m_transientOwnership)) {
        return Result::Busy;
    }
    if (!m_root || !m_tile || !generation || generation != m_generation) {
        return Result::StaleGeneration;
    }
    const auto activity = window ? activityContext(window) : std::nullopt;
    if (!window || !writer || !activity || key.isEmpty() || placementKey(window) != key || !freeOfNativeMembership(window)) {
        return Result::Ineligible;
    }
    Mutation mutation(this);
    const QPointer<TransientTileController> guard(this);
    window->ref();
    const auto lifetime = qScopeGuard([window]() {
        window->unref();
    });
    m_window = window;
    m_key = key;
    m_activity = *activity;
    if (!admissionCurrent(window, generation)) {
        clearAdmission();
        return Result::Ineligible;
    }
    const auto ownership = window->prepareTransientOwnership(writer, provider, key, providerRevision, mapRevision,
                                                             std::make_unique<Request>(this, window, generation));
    if (!ownership) {
        clearAdmission();
        return Result::Refused;
    }
    m_ownershipGeneration = ownership;
    window->m_transientOwnership->controller = this;
    m_windowClosed = connect(window, &Window::closed, this, [this, window, ownership]() {
        releaseWindow(window, ownership);
    });
    m_providerRetired = connect(writer, &TransientPlacementWriter::providerRetired, this, [this, window, provider, ownership](const QUuid &retired) {
        if (provider == retired) {
            releaseWindow(window, ownership);
        }
    });
    const auto id = window->internalId();
    m_keyConflict = connect(writer, &TransientPlacementWriter::keyConflict, this, [this, window, id, ownership](const QString &, const QUuid &owner, const QString &token) {
        if (owner == id && token == QString::number(ownership)) {
            releaseWindow(window, ownership);
        }
    });
    if (!window->activateTransientOwnership(ownership) || !m_tile->addMember(window)) {
        releaseWindow(window, ownership);
        return Result::Refused;
    }
    // The transient membership is recorded before its publication; a membership change or an
    // otherwise-valid public membership no-op made by a publication callback is newer intent that
    // no later fence of this admission would otherwise see.
    window->noteTileMembershipChanged();
    // The window outlives this call (retained above); the predicate reads its membership revision
    // through a pointer taken here so the tile needs no access of its own.
    const quint64 *const revision = &window->m_tileMembershipChange;
    const auto membership = *revision;
    const auto membershipCurrent = [revision, membership]() {
        return *revision == membership;
    };
    m_membershipPublished = true;
    if (!m_tile->publishAdded(window, generation, membershipCurrent)) {
        window->revokeTransientOwnership(ownership);
        return Result::Superseded;
    }
    const auto receipt = window->applyTransientOwnership(ownership);
    if (guard && guard->admissionCurrent(window, generation) && receipt.outcome == Window::TransientRequestOutcome::Applied) {
        return Result::Accepted;
    }
    window->revokeTransientOwnership(ownership);
    return Result::Superseded;
}

bool TransientTileController::releaseWindow(Window *window, quint64 generation, Window::AssignmentStepResult *guarded)
{
    if (m_window != window || m_ownershipGeneration != generation) {
        if (window && window->m_transientOwnership && window->m_transientOwnership->generation == generation
            && window->m_transientOwnership->controller == this) {
            window->m_transientOwnership->controller.clear();
            return window->revokeTransientOwnership(generation, guarded);
        }
        return false;
    }
    Mutation mutation(this);
    Release release(this, true, guarded);
    return release.finish();
}

quint64 TransientTileController::generation() const
{
    return m_root && m_tile ? m_generation : 0;
}

Tile *TransientTileController::rootTile() const
{
    return m_root;
}

Tile *TransientTileController::tile() const
{
    return m_tile;
}

} // namespace KWin

#include "moc_transienttilecontroller.cpp"
