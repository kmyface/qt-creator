// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "targetsettingspanel.h"

#include "buildmanager.h"
#include "buildsettingspropertiespage.h"
#include "kit.h"
#include "kitmanager.h"
#include "panelswidget.h"
#include "project.h"
#include "projectexplorer.h"
#include "projectexplorerconstants.h"
#include "projectexplorericons.h"
#include "projectexplorersettings.h"
#include "projectexplorertr.h"
#include "projectimporter.h"
#include "projectmanager.h"
#include "projectwindow.h"
#include "runsettingspropertiespage.h"
#include "target.h"
#include "targetsetuppage.h"
#include "task.h"

#include <coreplugin/icore.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/modemanager.h>

#include <utils/qtcassert.h>
#include <utils/stylehelper.h>
#include <utils/treemodel.h>
#include <utils/utilsicons.h>

#include <QCoreApplication>
#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <cmath>

using namespace Core;
using namespace Utils;

namespace ProjectExplorer {
namespace Internal {

class TargetSetupPageWrapper : public QWidget
{
public:
    explicit TargetSetupPageWrapper(Project *project);

    void ensureSetupPage()
    {
        if (!m_targetSetupPage)
            addTargetSetupPage();
    }

protected:
    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if ((m_targetSetupPage && m_targetSetupPage->importLineEditHasFocus())
                || (m_configureButton && !m_configureButton->isEnabled())) {
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            event->accept();
            if (m_targetSetupPage)
                done();
        }
    }

private:
    void done()
    {
        QTC_ASSERT(m_targetSetupPage, return);
        m_targetSetupPage->disconnect();
        m_targetSetupPage->setupProject(m_project);
        m_targetSetupPage->deleteLater();
        m_targetSetupPage = nullptr;
        Core::ModeManager::activateMode(Core::Constants::MODE_EDIT);
    }

    void completeChanged()
    {
        m_configureButton->setEnabled(m_targetSetupPage && m_targetSetupPage->isComplete());
    }

    void addTargetSetupPage();

    Project * const m_project;
    TargetSetupPage *m_targetSetupPage = nullptr;
    QPushButton *m_configureButton = nullptr;
    QVBoxLayout *m_setupPageContainer = nullptr;
};

TargetSetupPageWrapper::TargetSetupPageWrapper(Project *project)
    : m_project(project)
{
    auto box = new QDialogButtonBox(this);

    m_configureButton = new QPushButton(this);
    m_configureButton->setText(Tr::tr("&Configure Project"));
    box->addButton(m_configureButton, QDialogButtonBox::AcceptRole);

    auto hbox = new QHBoxLayout;
    hbox->addStretch();
    hbox->addWidget(box);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_setupPageContainer = new QVBoxLayout;
    layout->addLayout(m_setupPageContainer);
    layout->addLayout(hbox);
    completeChanged();
    connect(m_configureButton, &QAbstractButton::clicked,
            this, &TargetSetupPageWrapper::done);
}

void TargetSetupPageWrapper::addTargetSetupPage()
{
    m_targetSetupPage = new TargetSetupPage(this);
    m_targetSetupPage->setProjectPath(m_project->projectFilePath());
    m_targetSetupPage->setTasksGenerator(
        [this](const Kit *k) { return m_project->projectIssues(k); });
    m_targetSetupPage->setProjectImporter(m_project->projectImporter());
    m_targetSetupPage->initializePage();
    m_targetSetupPage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_setupPageContainer->addWidget(m_targetSetupPage);

    completeChanged();

    connect(m_targetSetupPage, &QWizardPage::completeChanged,
            this, &TargetSetupPageWrapper::completeChanged);
}

//
// TargetSettingsPanelItem
//

class TargetGroupItemPrivate : public QObject
{
public:
    TargetGroupItemPrivate(TargetGroupItem *q, Project *project);
    ~TargetGroupItemPrivate() override;

    void handleAddedKit(Kit *kit);

    void handleTargetAdded(Target *target);
    void handleTargetRemoved(Target *target);
    void handleTargetChanged(Target *target);

    void ensureWidget();
    void scheduleRebuildContents();
    void rebuildContents();
    void ensureShowMoreItem();

    void setShowAllKits(bool showAllKits)
    {
        QtcSettings *settings = Core::ICore::settings();
        settings->setValue(ProjectExplorer::Constants::SHOW_ALL_KITS_SETTINGS_KEY, showAllKits);
        mutableProjectExplorerSettings().showAllKits = showAllKits;
        rebuildContents();
    }
    bool showAllKits() const
    {
        return projectExplorerSettings().showAllKits;
    }

    TargetGroupItem * const q;
    Project * const m_project;
    QString m_displayName;
    bool m_rebuildScheduled = false;

    QPointer<QWidget> m_noKitLabel;
    QPointer<QWidget> m_configurePage;
    QPointer<QWidget> m_configuredPage;
    TargetSetupPageWrapper *m_targetSetupPageWrapper = nullptr;
    QList<QMetaObject::Connection> m_connections;
};

class ITargetItem : public TypedTreeItem<TreeItem, TargetGroupItem>
{
public:
    enum { DefaultPage = 0 }; // Build page.

    ITargetItem(Project *project, Id kitId, const Tasks &issues)
        : m_project(project)
        , m_kitId(kitId)
        , m_kitIssues(issues)
    {}

    virtual Target *target() const = 0;
    virtual void updateSubItems() = 0;
    virtual void addToContextMenu(QMenu *menu, bool isSelectable) = 0;

    bool isEnabled() const { return target() != nullptr; }

public:
    QPointer<Project> m_project; // Not owned.
    Id m_kitId;
    int m_currentChild = DefaultPage;
    bool m_kitErrorsForProject = false;
    bool m_kitWarningForProject = false;
    Tasks m_kitIssues;
};

class ShowMoreItem : public ITargetItem
{
public:
    ShowMoreItem(TargetGroupItemPrivate *p)
        : ITargetItem(nullptr, Id(), Tasks())
        , m_p(p)
    {}

    QVariant data(int column, int role) const override
    {
        Q_UNUSED(column)
        if (role == Qt::DisplayRole) {
            return !m_p->showAllKits() ? Tr::tr("Show All Kits") : Tr::tr("Hide Inactive Kits");
        }

        if (role == IsShowMoreRole)
            return true;

        return {};
    }

    bool setData(int column, const QVariant &data, int role) override
    {
        Q_UNUSED(column)
        Q_UNUSED(data)
        if (role == ItemActivatedDirectlyRole) {
            m_p->setShowAllKits(!m_p->showAllKits());
            return true;
        }
        return false;
    }

    Qt::ItemFlags flags(int) const override
    {
        return Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsSelectable;
    }

    // ITargetItem
    Target *target() const override {
        return nullptr;
    }
    void updateSubItems() override {}
    void addToContextMenu(QMenu *, bool) override {}

private:
    TargetGroupItemPrivate *m_p;
};

void TargetGroupItemPrivate::ensureWidget()
{
    if (!m_noKitLabel) {
        m_noKitLabel = new QWidget;
        m_noKitLabel->setFocusPolicy(Qt::NoFocus);

        auto label = new QLabel;
        label->setText(Tr::tr("No kit defined in this project."));
        label->setFont(StyleHelper::uiFont(StyleHelper::UiElementH4));
        label->setContentsMargins(10, 10, 10, 10);
        label->setAlignment(Qt::AlignTop);

        auto layout = new QVBoxLayout(m_noKitLabel);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(label);
        layout->addStretch(10);
    }

    if (!m_configurePage) {
        m_targetSetupPageWrapper = new TargetSetupPageWrapper(m_project);
        m_configurePage
            = new PanelsWidget(Tr::tr("Configure Project"), m_targetSetupPageWrapper, false);
        m_configurePage->setFocusProxy(m_targetSetupPageWrapper);
    }
    m_targetSetupPageWrapper->ensureSetupPage();

    if (!m_configuredPage) {
        auto widget = new QWidget;
        auto label = new QLabel("This project is already configured.");
        auto layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(label);
        layout->addStretch(10);
        m_configuredPage = new PanelsWidget(Tr::tr("Configure Project"), widget);
    }
}

//
// Third level: The per-kit entries (inactive or with a 'Build' and a 'Run' subitem)
//
class TargetItem : public ITargetItem
{
public:
    TargetItem(Project *project, Id kitId, const Tasks &issues)
        : ITargetItem(project, kitId, issues)
    {
        m_kitWarningForProject = containsType(m_kitIssues, Task::TaskType::Warning);
        m_kitErrorsForProject = containsType(m_kitIssues, Task::TaskType::Error);

        updateSubItems();
    }

    Target *target() const override
    {
        return m_project->target(m_kitId);
    }

    void updateSubItems() override;

    Qt::ItemFlags flags(int column) const override
    {
        Q_UNUSED(column)
        return m_kitErrorsForProject ? Qt::ItemFlags({})
                                     : Qt::ItemFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    }

    QVariant data(int column, int role) const override
    {
        switch (role) {
        case Qt::DisplayRole: {
            if (Kit *kit = KitManager::kit(m_kitId))
                return kit->displayName();
            break;
        }

        case Qt::DecorationRole: {
            const Kit *k = KitManager::kit(m_kitId);
            if (!k)
                break;
            if (m_kitErrorsForProject)
                return kitIconWithOverlay(*k, IconOverlay::Error);
            if (!isEnabled())
                return kitIconWithOverlay(*k, IconOverlay::Add);
            if (m_kitWarningForProject)
                return kitIconWithOverlay(*k, IconOverlay::Warning);
            return k->icon();
        }

        case Qt::ForegroundRole: {
            if (!isEnabled())
                return Utils::creatorColor(Theme::TextColorDisabled);
            break;
        }

        case Qt::FontRole: {
            QFont font = parent()->data(column, role).value<QFont>();
            if (ITargetItem *targetItem = parent()->currentTargetItem()) {
                Target *t = targetItem->target();
                if (t && t->id() == m_kitId && m_project == ProjectManager::startupProject())
                    font.setBold(true);
            }
            return font;
        }

        case Qt::ToolTipRole: {
            Kit *k = KitManager::kit(m_kitId);
            if (!k)
                break;
            const QString extraText = [this] {
                if (m_kitErrorsForProject)
                    return QString("<h3>" + Tr::tr("Kit is unsuited for project") + "</h3>");
                if (!isEnabled())
                    return QString("<h3>" + Tr::tr("Click to activate") + "</h3>");
                return QString();
            }();
            return k->toHtml(m_kitIssues, extraText);
        }

        case PanelWidgetRole:
        case ActiveItemRole: {
            if (m_currentChild >= 0 && m_currentChild < childCount())
                return childAt(m_currentChild)->data(column, role);
            break;
        }

        default:
            break;
        }
        return {};
    }

    bool setData(int column, const QVariant &data, int role) override
    {
        Q_UNUSED(column)

        if (role == ContextMenuItemAdderRole) {
            auto *menu = data.value<QMenu *>();
            addToContextMenu(menu, flags(column) & Qt::ItemIsSelectable);
            return true;
        }

        if (role == ItemActivatedDirectlyRole) {
            QTC_ASSERT(!data.isValid(), return false);
            if (!isEnabled()) {
                m_currentChild = DefaultPage;
                m_project->addTargetForKit(KitManager::kit(m_kitId));
            } else {
                // Go to Run page, when on Run previously etc.
                ITargetItem *previousItem = parent()->currentTargetItem();
                m_currentChild = previousItem ? previousItem->m_currentChild : DefaultPage;
                m_project->setActiveTarget(target(), SetActive::Cascade);
                parent()->setData(column, QVariant::fromValue(static_cast<TreeItem *>(this)),
                                  ItemActivatedFromBelowRole);
            }
            return true;
        }

        if (role == ItemActivatedFromBelowRole) {
            // I.e. 'Build' and 'Run' items were present and user clicked on them.
            int child = indexOf(data.value<TreeItem *>());
            QTC_ASSERT(child != -1, return false);
            m_currentChild = child; // Triggered from sub-item.
            m_project->setActiveTarget(target(), SetActive::Cascade);
            // Propagate Build/Run selection up.
            parent()->setData(column, QVariant::fromValue(static_cast<TreeItem *>(this)),
                              ItemActivatedFromBelowRole);
            return true;
        }

        if (role == ItemActivatedFromAboveRole) {
            // Usually programmatic activation, e.g. after opening the Project mode.
            m_project->setActiveTarget(target(), SetActive::Cascade);
            return true;
        }
        return false;
    }

    void addToContextMenu(QMenu *menu, bool isSelectable) override
    {
        Kit *kit = KitManager::kit(m_kitId);
        QTC_ASSERT(kit, return);
        const QString projectName = m_project->displayName();

        QAction *enableAction = menu->addAction(Tr::tr("Enable Kit for Project \"%1\"").arg(projectName));
        enableAction->setEnabled(isSelectable && m_kitId.isValid() && !isEnabled());
        QObject::connect(enableAction, &QAction::triggered, [this, kit] {
            m_project->addTargetForKit(kit);
        });

        QAction * const enableForAllAction
                = menu->addAction(Tr::tr("Enable Kit for All Projects"));
        enableForAllAction->setEnabled(isSelectable);
        QObject::connect(enableForAllAction, &QAction::triggered, [kit] {
            for (Project * const p : ProjectManager::projects()) {
                if (!p->target(kit))
                    p->addTargetForKit(kit);
            }
        });

        QAction *disableAction = menu->addAction(Tr::tr("Disable Kit for Project \"%1\"").arg(projectName));
        disableAction->setEnabled(isSelectable && m_kitId.isValid() && isEnabled());
        QObject::connect(disableAction, &QAction::triggered, m_project, [this] {
            Target *t = target();
            QTC_ASSERT(t, return);
            QString kitName = t->displayName();
            if (BuildManager::isBuilding(t)) {
                QMessageBox box;
                QPushButton *closeAnyway = box.addButton(Tr::tr("Cancel Build and Disable Kit in This Project"), QMessageBox::AcceptRole);
                QPushButton *cancelClose = box.addButton(Tr::tr("Do Not Remove"), QMessageBox::RejectRole);
                box.setDefaultButton(cancelClose);
                box.setWindowTitle(Tr::tr("Disable Kit \"%1\" in This Project?").arg(kitName));
                box.setText(Tr::tr("The kit <b>%1</b> is currently being built.").arg(kitName));
                box.setInformativeText(Tr::tr("Do you want to cancel the build process and remove the kit anyway?"));
                box.exec();
                if (box.clickedButton() != closeAnyway)
                    return;
                BuildManager::cancel();
            }

            QCoreApplication::processEvents();

            m_project->removeTarget(t);
        });

        QAction *disableForAllAction = menu->addAction(Tr::tr("Disable Kit for All Projects"));
        disableForAllAction->setEnabled(isSelectable);
        QObject::connect(disableForAllAction, &QAction::triggered, [kit] {
            for (Project * const p : ProjectManager::projects()) {
                Target * const t = p->target(kit);
                if (!t)
                    continue;
                if (BuildManager::isBuilding(t))
                    BuildManager::cancel();
                p->removeTarget(t);
            }
        });

        QMenu *copyMenu = menu->addMenu(Tr::tr("Copy Steps From Another Kit..."));
        if (m_kitId.isValid() && m_project->target(m_kitId)) {
            const QList<Kit *> kits = KitManager::kits();
            for (Kit *kit : kits) {
                QAction *copyAction = copyMenu->addAction(kit->displayName());
                if (kit->id() == m_kitId || !m_project->target(kit->id())) {
                    copyAction->setEnabled(false);
                } else {
                    QObject::connect(copyAction, &QAction::triggered, [this, kit] {
                        Target *thisTarget = m_project->target(m_kitId);
                        Target *sourceTarget = m_project->target(kit->id());
                        Project::copySteps(sourceTarget, thisTarget);
                    });
                }
            }
        } else {
            copyMenu->setEnabled(false);
        }
    }

private:
    enum class IconOverlay {
        Add,
        Warning,
        Error
    };

    static QIcon kitIconWithOverlay(const Kit &kit, IconOverlay overlayType)
    {
        QIcon overlayIcon;
        switch (overlayType) {
        case IconOverlay::Add: {
            static const QIcon add = Utils::Icons::OVERLAY_ADD.icon();
            overlayIcon = add;
            break;
        }
        case IconOverlay::Warning: {
            static const QIcon warning = Utils::Icons::OVERLAY_WARNING.icon();
            overlayIcon = warning;
            break;
        }
        case IconOverlay::Error: {
            static const QIcon err = Utils::Icons::OVERLAY_ERROR.icon();
            overlayIcon = err;
            break;
        }
        }
        const QSize iconSize(16, 16);
        const QRect iconRect(QPoint(), iconSize);
        QPixmap result(iconSize * qApp->devicePixelRatio());
        result.fill(Qt::transparent);
        result.setDevicePixelRatio(qApp->devicePixelRatio());
        QPainter p(&result);
        kit.icon().paint(&p, iconRect, Qt::AlignCenter,
                         overlayType == IconOverlay::Add ? QIcon::Disabled : QIcon::Normal);
        overlayIcon.paint(&p, iconRect);
        return result;
    }
};

//
// Fourth level: The 'Build' and 'Run' sub-items.
//

class BuildOrRunItem : public TreeItem
{
public:
    enum SubIndex { BuildPage = 0, RunPage = 1 };

    BuildOrRunItem(Project *project, Id kitId, SubIndex subIndex)
        : m_project(project), m_kitId(kitId), m_subIndex(subIndex)
    { }

    ~BuildOrRunItem() override
    {
        delete m_panel;
    }

    Target *target() const
    {
        return m_project->target(m_kitId);
    }

    QVariant data(int column, int role) const override
    {
        switch (role) {
        case Qt::DisplayRole: {
            switch (m_subIndex) {
            case BuildPage:
                return Tr::tr("Build");
            case RunPage:
                return Tr::tr("Run");
            }
            break;
        }

        case Qt::ToolTipRole:
            return parent()->data(column, role);

        case PanelWidgetRole:
            return QVariant::fromValue(panel());

        case ActiveItemRole:
            return QVariant::fromValue<TreeItem *>(const_cast<BuildOrRunItem *>(this));

        case KitIdRole:
            return m_kitId.toSetting();

        case Qt::DecorationRole: {
            switch (m_subIndex) {
            case BuildPage: {
                static const QIcon buildIcon = ProjectExplorer::Icons::BUILD_SMALL.icon();
                return buildIcon;
            }
            case RunPage: {
                static const QIcon runIcon = Utils::Icons::RUN_SMALL.icon();
                return runIcon;
            }
            }
            break;
        }

        default:
            break;
        }
        return {};
    }

    Qt::ItemFlags flags(int column) const override
    {
        return parent()->flags(column);
    }

    bool setData(int column, const QVariant &data, int role) override
    {
        if (role == ItemActivatedDirectlyRole) {
            parent()->setData(column, QVariant::fromValue(static_cast<TreeItem *>(this)),
                              ItemActivatedFromBelowRole);
            return true;
        }

        return parent()->setData(column, data, role);
    }

    QWidget *panel() const
    {
        if (!m_panel) {
            m_panel = (m_subIndex == RunPage)
                    ? new PanelsWidget(Tr::tr("Run Settings"), createRunSettingsWidget(target()))
                    : new PanelsWidget(Tr::tr("Build Settings"), createBuildSettingsWidget(target()));
        }
        return m_panel;
    }

public:
    Project *m_project; // Not owned.
    Id m_kitId;
    mutable QPointer<QWidget> m_panel; // Owned.
    const SubIndex m_subIndex;
};

//
// Also third level:
//

TargetGroupItem::TargetGroupItem(const QString &displayName, Project *project)
    : d(std::make_unique<TargetGroupItemPrivate>(this, project))
{
    d->m_displayName = displayName;
}

TargetGroupItem::~TargetGroupItem() = default;

TargetGroupItemPrivate::TargetGroupItemPrivate(TargetGroupItem *q, Project *project)
    : q(q), m_project(project)
{
    m_connections << QObject::connect(project, &Project::addedTarget,
                                      this, &TargetGroupItemPrivate::handleTargetAdded);
    m_connections << QObject::connect(project, &Project::removedTarget,
                                      this, &TargetGroupItemPrivate::handleTargetRemoved);
    m_connections << QObject::connect(project, &Project::activeTargetChanged,
                                      this, &TargetGroupItemPrivate::handleTargetChanged);

    // force a signal since the index has changed
    m_connections << connect(KitManager::instance(), &KitManager::kitAdded,
                             this, &TargetGroupItemPrivate::handleAddedKit);
    m_connections << connect(KitManager::instance(), &KitManager::kitRemoved,
                             this, &TargetGroupItemPrivate::scheduleRebuildContents);
    m_connections << connect(KitManager::instance(), &KitManager::kitUpdated,
                             this, &TargetGroupItemPrivate::scheduleRebuildContents);
    m_connections << connect(KitManager::instance(), &KitManager::kitsLoaded,
                             this, &TargetGroupItemPrivate::scheduleRebuildContents);
    m_connections << connect(
        ProjectExplorerPlugin::instance(),
        &ProjectExplorerPlugin::settingsChanged,
        this,
        &TargetGroupItemPrivate::scheduleRebuildContents);

    rebuildContents();
}

TargetGroupItemPrivate::~TargetGroupItemPrivate()
{
    disconnect();
    for (const QMetaObject::Connection & c : std::as_const(m_connections))
        disconnect(c);

    delete m_noKitLabel;
    delete m_configurePage;
    delete m_configuredPage;
}

QVariant TargetGroupItem::data(int column, int role) const
{
    if (role == Qt::DisplayRole)
        return d->m_displayName;

    if (role == ActiveItemRole) {
        if (ITargetItem *item = currentTargetItem())
            return item->data(column, role);
        return QVariant::fromValue<TreeItem *>(const_cast<TargetGroupItem *>(this));
    }

    if (role == PanelWidgetRole) {
        if (ITargetItem *item = currentTargetItem())
            return item->data(column, role);

        d->ensureWidget();
        return QVariant::fromValue<QWidget *>(d->m_configurePage.data());
    }
    return {};
}

bool TargetGroupItem::setData(int column, const QVariant &data, int role)
{
    Q_UNUSED(data)
    if (role == ItemActivatedFromBelowRole || role == ItemUpdatedFromBelowRole) {
        // Bubble up to trigger setting the active project.
        QTC_ASSERT(parent(), return false);
        parent()->setData(column, QVariant::fromValue(static_cast<TreeItem *>(this)), role);
        return true;
    }

    return false;
}

Qt::ItemFlags TargetGroupItem::flags(int) const
{
    return Qt::NoItemFlags;
}

ITargetItem *TargetGroupItem::currentTargetItem() const
{
    return targetItem(d->m_project->activeTarget());
}

TreeItem *TargetGroupItem::buildSettingsItem() const
{
    if (ITargetItem * const targetItem = currentTargetItem()) {
        if (targetItem->childCount() == 2)
            return targetItem->childAt(0);
    }
    return nullptr;
}

TreeItem *TargetGroupItem::runSettingsItem() const
{
    if (ITargetItem * const targetItem = currentTargetItem()) {
        if (targetItem->hasChildren())
            return targetItem->childAt(targetItem->childCount() - 1);
    }
    return nullptr;
}

ITargetItem *TargetGroupItem::targetItem(Target *target) const
{
    if (target) {
        Id needle = target->id(); // Unconfigured project have no active target.
        return findFirstLevelChild([needle](ITargetItem *item) { return item->m_kitId == needle; });
    }
    return nullptr;
}

void TargetGroupItemPrivate::handleAddedKit(Kit *kit)
{
    q->appendChild(new TargetItem(m_project, kit->id(), m_project->projectIssues(kit)));
    scheduleRebuildContents();
}

void TargetItem::updateSubItems()
{
    if (childCount() == 0 && isEnabled())
        m_currentChild = DefaultPage; // We will add children below.
    removeChildren();
    if (isEnabled() && !m_kitErrorsForProject) {
        if (m_project->supportsBuilding())
            appendChild(new BuildOrRunItem(m_project, m_kitId, BuildOrRunItem::BuildPage));
        appendChild(new BuildOrRunItem(m_project, m_kitId, BuildOrRunItem::RunPage));
    }
}

void TargetGroupItemPrivate::ensureShowMoreItem()
{
    if (q->findAnyChild([](TreeItem *item) { return item->data(0, IsShowMoreRole).toBool(); }))
        return;

    q->appendChild(new ShowMoreItem(this));
}

void TargetGroupItemPrivate::scheduleRebuildContents()
{
    if (m_rebuildScheduled)
        return;
    m_rebuildScheduled = true;
    QMetaObject::invokeMethod(this, &TargetGroupItemPrivate::rebuildContents, Qt::QueuedConnection);
}

void TargetGroupItemPrivate::rebuildContents()
{
    m_rebuildScheduled = false;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const auto sortedKits = KitManager::sortedKits();
    bool isAnyKitNotEnabled = std::any_of(sortedKits.begin(), sortedKits.end(), [this](Kit *kit) {
        return kit && m_project->target(kit->id()) != nullptr;
    });
    q->removeChildren();

    for (Kit *kit : sortedKits) {
        if (!isAnyKitNotEnabled || showAllKits() || m_project->target(kit->id()) != nullptr)
            q->appendChild(new TargetItem(m_project, kit->id(), m_project->projectIssues(kit)));
    }

    if (isAnyKitNotEnabled)
        ensureShowMoreItem();

    if (q->parent()) {
        q->parent()
            ->setData(0, QVariant::fromValue(static_cast<TreeItem *>(q)), ItemUpdatedFromBelowRole);
    }

    QGuiApplication::restoreOverrideCursor();
}

void TargetGroupItemPrivate::handleTargetAdded(Target *target)
{
    if (ITargetItem *item = q->targetItem(target))
        item->updateSubItems();
    ensureShowMoreItem();
    q->update();
}

void TargetGroupItemPrivate::handleTargetRemoved(Target *target)
{
    if (ITargetItem *item = q->targetItem(target))
        item->updateSubItems();
    ensureShowMoreItem();
    QTC_ASSERT(q->parent(), qDebug() << m_displayName; return);
    q->parent()->setData(0, QVariant::fromValue(static_cast<TreeItem *>(q)),
                         ItemDeactivatedFromBelowRole);
}

void TargetGroupItemPrivate::handleTargetChanged(Target *target)
{
    if (ITargetItem *item = q->targetItem(target))
        item->updateSubItems();
    ensureShowMoreItem();
    q->setData(0, QVariant(), ItemActivatedFromBelowRole);
}

} // Internal
} // ProjectExplorer
