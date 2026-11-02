/*
    SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "transienttilecontroller.h"

#include "tile.h"
#include "tilemanager.h"
#include "virtualdesktops.h"

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
        if (m_controller) {
            m_controller->nodeAboutToBeDestroyed(this);
        }
    }

private:
    friend class TransientTileController;

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
};

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
    const auto root = m_root;
    resetTree();
    if (root) {
        root->setParent(nullptr);
    }
    delete root.data();
}

TransientTileController::Result TransientTileController::createTree(TileManager *manager, VirtualDesktop *desktop, const RectF &geometry)
{
    if (m_mutating || m_shuttingDown || m_root || m_tile) {
        return Result::Busy;
    }
    if (!validContext(manager, desktop)) {
        return Result::InvalidContext;
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
    if (m_mutating || m_shuttingDown) {
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
    const auto root = m_root;
    resetTree();
    QPointer<TransientTileController> guard(this);
    root->setParent(nullptr);
    delete root.data();
    if (!guard) {
        return Result::Superseded;
    }
    Q_EMIT treeChanged();
    return guard ? Result::Accepted : Result::Superseded;
}

void TransientTileController::resetTree()
{
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
    if (guard) {
        QMetaObject::invokeMethod(this, [this, generation]() {
            if (!m_shuttingDown && m_generation == generation) {
                Q_EMIT treeChanged();
            }
        }, Qt::QueuedConnection);
    }
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
