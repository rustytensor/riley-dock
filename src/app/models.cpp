// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models.h"

#include <riley-dock/autohide.h>

namespace riley::dock::app {

// --- DockItems ---------------------------------------------------------------------------------------------------

int DockItems::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant DockItems::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const ItemData &row = m_rows[index.row()];
    switch (role) {
    case ItemIdRole: return row.itemId;
    case NameRole: return row.name;
    case IconSourceRole: return row.iconSource;
    case SymbolicRole: return row.symbolic;
    case LauncherRole: return row.launcher;
    case RunningRole: return row.running;
    case FocusedRole: return row.focused;
    case MinimizedRole: return row.minimized;
    case IndicatorRole: return row.indicator;
    case DotsRole: return row.dots;
    case RectRole: return row.rect;
    case DrawRectRole: return row.drawRect;
    case BounceRole: return row.bounce;
    case ScaleRole: return row.scale;
    case HoveredRole: return row.hovered;
    case ShadowRole: return row.shadow;
    case ReflectedRole: return row.reflected;
    case ReflectionImageRole: return row.reflectionImage;
    case ReflectionClipRole: return row.reflectionClip;
    case ReflectionFlipYRole: return row.reflectionFlipY;
    case ReflectionStopsRole: return row.reflectionStops;
    case GridDotsRole: return row.gridDots;
    case GridDotRadiusRole: return row.gridDotRadius;
    case GridAccentRole: return row.gridAccent;
    case GridSpacingRole: return row.gridSpacing;
    case PinnedRole: return row.pinned;
    default: return {};
    }
}

QHash<int, QByteArray> DockItems::roleNames() const
{
    return {
        {ItemIdRole, "itemId"}, {NameRole, "name"}, {IconSourceRole, "iconSource"}, {SymbolicRole, "symbolic"},
        {LauncherRole, "launcher"}, {RunningRole, "running"}, {FocusedRole, "focused"},
        {MinimizedRole, "minimized"}, {IndicatorRole, "indicator"}, {DotsRole, "dots"}, {RectRole, "rect"},
        {DrawRectRole, "drawRect"}, {BounceRole, "bounce"}, {ScaleRole, "scale"}, {HoveredRole, "hovered"},
        {ShadowRole, "shadow"}, {ReflectedRole, "reflected"}, {ReflectionImageRole, "reflectionImage"},
        {ReflectionClipRole, "reflectionClip"}, {ReflectionFlipYRole, "reflectionFlipY"},
        {ReflectionStopsRole, "reflectionStops"}, {GridDotsRole, "gridDots"}, {GridDotRadiusRole, "gridDotRadius"},
        {GridAccentRole, "gridAccent"}, {GridSpacingRole, "gridSpacing"}, {PinnedRole, "pinned"},
    };
}

void DockItems::set(const QList<ItemData> &rows)
{
    bool sameIds = rows.size() == m_rows.size();
    for (qsizetype i = 0; sameIds && i < rows.size(); ++i)
        sameIds = rows[i].itemId == m_rows[i].itemId;
    if (!sameIds) {
        const bool countChanges = rows.size() != m_rows.size();
        beginResetModel();
        m_rows = rows;
        endResetModel();
        if (countChanges)
            Q_EMIT countChanged();
        return;
    }
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (rows[i] == m_rows[i])
            continue;
        m_rows[i] = rows[i];
        Q_EMIT dataChanged(index(int(i)), index(int(i)));
    }
}

// --- DockMenu ----------------------------------------------------------------------------------------------------

QStringList DockMenu::actions() const
{
    // riley dock/src/ui.rs:816-818, in this order; DockApp maps the index back to ipc::WindowAction.
    QStringList labels{QStringLiteral("Close"), QStringLiteral("Float/tile"), QStringLiteral("Full screen")};
    for (int n = 1; n <= 9; ++n)
        labels.append(QStringLiteral("Move to workspace %1").arg(n));
    return labels;
}

void DockMenu::show(const QString &kind, const QRect &anchor, const QString &itemId, const QVariantList &windows,
                    bool pinned)
{
    m_open = true;
    m_kind = kind;
    m_anchor = anchor;
    m_itemId = itemId;
    m_windows = windows;
    m_pinned = pinned;
    Q_EMIT changed();
}

void DockMenu::hide()
{
    if (!m_open)
        return;
    m_open = false;
    Q_EMIT changed();
    if (onClosed)
        onClosed();
}

void DockMenu::pickWindow(int index)
{
    if (!m_open || m_kind != QStringLiteral("windows") || index < 0 || index >= m_windows.size())
        return dismiss();
    hide();
    if (onPick)
        onPick(index);
}

void DockMenu::windowAction(int window, int action)
{
    if (!m_open || m_kind != QStringLiteral("context") || window < 0 || window >= m_windows.size() || action < 0
        || action >= actions().size())
        return dismiss();
    hide();
    if (onWindowAction)
        onWindowAction(window, action);
}

void DockMenu::togglePin()
{
    if (!m_open || m_kind != QStringLiteral("context"))
        return dismiss();
    hide();
    if (onTogglePin)
        onTogglePin();
}

void DockMenu::dismiss()
{
    hide();
}

// --- DockModel ---------------------------------------------------------------------------------------------------

DockModel::DockModel(DockStyle *style, QString output, QObject *parent)
    : QObject(parent), m_style(style), m_output(std::move(output))
{
    m_items.setParent(this);
    m_menu.setParent(this);
}

double DockModel::lipDotRadius() const
{
    return autohide::DOT_RADIUS;
}

void DockModel::setScene(const Scene &scene, const QList<ItemData> &items)
{
    m_items.set(items);
    if (scene == m_scene)
        return;
    m_scene = scene;
    Q_EMIT sceneChanged();
}

void DockModel::setHover(const QString &text, bool visible)
{
    if (text == m_hoverText && visible == m_hoverVisible)
        return;
    m_hoverText = text;
    m_hoverVisible = visible;
    Q_EMIT hoverChanged();
}

} // namespace riley::dock::app
