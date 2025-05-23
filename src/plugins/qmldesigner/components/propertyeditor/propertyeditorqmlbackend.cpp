// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "propertyeditorqmlbackend.h"

#include "instanceimageprovider.h"
#include "propertyeditortransaction.h"
#include "propertyeditorutils.h"
#include "propertyeditorvalue.h"
#include "propertymetainfo.h"

#include "componentcore/utils3d.h"

#include <auxiliarydataproperties.h>
#include <bindingproperty.h>
#include <nodemetainfo.h>
#include <qmldesignerconstants.h>
#include <qmldesignerplugin.h>
#include <qmldesignertr.h>
#include <qmlobjectnode.h>
#include <qmltimeline.h>
#include <sourcepathcache.h>
#include <variantproperty.h>

#include <theme.h>

#include <qmldesignerbase/settings/designersettings.h>

#include <coreplugin/icore.h>
#include <coreplugin/messagebox.h>
#include <qmljs/qmljssimplereader.h>
#include <utils/algorithm.h>
#include <utils/array.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/smallstring.h>

#ifdef QDS_USE_PROJECTSTORAGE
#include <qmlprojectmanager/qmlproject.h>
#endif

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QQuickItem>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <QLoggingCategory>

#include <tuple>

static Q_LOGGING_CATEGORY(propertyEditorBenchmark, "qtc.propertyeditor.load", QtWarningMsg)

static QmlJS::SimpleReaderNode::Ptr s_templateConfiguration = QmlJS::SimpleReaderNode::Ptr();

inline static QString propertyTemplatesPath()
{
    return QmlDesigner::PropertyEditorQmlBackend::propertyEditorResourcesPath() + QStringLiteral("/PropertyTemplates/");
}

QmlJS::SimpleReaderNode::Ptr templateConfiguration()
{
    if (!s_templateConfiguration) {
        QmlJS::SimpleReader reader;
        const QString fileName = propertyTemplatesPath() + QStringLiteral("TemplateTypes.qml");
        s_templateConfiguration = reader.readFile(fileName);

        if (!s_templateConfiguration)
            qWarning().nospace() << "template definitions:" << reader.errors();
    }

    return s_templateConfiguration;
}

QStringList variantToStringList(const QVariant &variant) {
    QStringList stringList;

    for (const QVariant &singleValue : variant.toList())
        stringList << singleValue.toString();

    return stringList;
}

static QObject *variantToQObject(const QVariant &value)
{
    if (value.typeId() == QMetaType::QObjectStar || value.typeId() > QMetaType::User)
        return *(QObject **)value.constData();

    return nullptr;
}

namespace QmlDesigner {

using namespace Qt::StringLiterals;

static bool isMaterialAuxiliaryKey(AuxiliaryDataKeyView key)
{
    static constexpr auto previewKeys = Utils::to_array<AuxiliaryDataKeyView>(
        materialPreviewEnvDocProperty,
        materialPreviewEnvValueDocProperty,
        materialPreviewModelDocProperty,
        materialPreviewEnvProperty,
        materialPreviewEnvValueProperty,
        materialPreviewModelProperty);

    return std::ranges::find(previewKeys, key) != std::ranges::end(previewKeys);
}

PropertyEditorQmlBackend::PropertyEditorQmlBackend(PropertyEditorView *propertyEditor,
                                                   AsynchronousImageCache &imageCache)
    : m_contextObject(std::make_unique<PropertyEditorContextObject>())
    , m_view(Utils::makeUniqueObjectPtr<Quick2PropertyEditorView>(imageCache))
    , m_propertyEditorTransaction(std::make_unique<PropertyEditorTransaction>(propertyEditor))
    , m_dummyPropertyEditorValue(std::make_unique<PropertyEditorValue>())
{
    m_contextObject->setQuickWidget(m_view.get());
    m_view->engine()->setOutputWarningsToStandardError(QmlDesignerPlugin::instance()
        ->settings().value(DesignerSettingsKey::SHOW_PROPERTYEDITOR_WARNINGS).toBool());

    m_view->engine()->addImportPath(propertyEditorResourcesPath() + "/imports");
    m_view->engine()->addImportPath(scriptsEditorResourcesPath() + "/imports");

    m_dummyPropertyEditorValue->setValue(QLatin1String("#000000"));
    context()->setContextProperty(QLatin1String("dummyBackendValue"),
                                  m_dummyPropertyEditorValue.get());
    m_contextObject->setBackendValues(&m_backendValuesPropertyMap);
    m_contextObject->setModel(propertyEditor->model());
    m_contextObject->insertInQmlContext(context());

    QObject::connect(&m_backendValuesPropertyMap, &DesignerPropertyMap::valueChanged,
                     propertyEditor, &PropertyEditorView::changeValue);
}

PropertyEditorQmlBackend::~PropertyEditorQmlBackend() = default;

void PropertyEditorQmlBackend::setupPropertyEditorValue(PropertyNameView name,
                                                        PropertyEditorView *propertyEditor,
                                                        const NodeMetaInfo &type)
{
    QmlDesigner::PropertyName propertyName(name.toByteArray());
    propertyName.replace('.', '_');
    auto valueObject = qobject_cast<PropertyEditorValue*>(variantToQObject(backendValuesPropertyMap().value(QString::fromUtf8(propertyName))));
    if (!valueObject) {
        valueObject = new PropertyEditorValue(&backendValuesPropertyMap());
        QObject::connect(valueObject, &PropertyEditorValue::valueChanged, &backendValuesPropertyMap(), &DesignerPropertyMap::valueChanged);
        QObject::connect(valueObject, &PropertyEditorValue::expressionChanged, propertyEditor, &PropertyEditorView::changeExpression);
        backendValuesPropertyMap().insert(QString::fromUtf8(propertyName), QVariant::fromValue(valueObject));
    }
    valueObject->setName(propertyName);
    if (type.isColor())
        valueObject->setValue(QVariant(QLatin1String("#000000")));
    else
        valueObject->setValue(QVariant(1));
}
namespace {
PropertyName auxNamePostFix(Utils::SmallStringView propertyName)
{
    return PropertyNameView(propertyName) + "__AUX";
}

QVariant properDefaultAuxiliaryProperties(const QmlObjectNode &qmlObjectNode,
                                          AuxiliaryDataKeyDefaultValue key)
{
    const ModelNode node = qmlObjectNode.modelNode();

    if (auto data = node.auxiliaryData(key))
        return *data;

    return getDefaultValueAsQVariant(key);
}

QVariant properDefaultLayoutAttachedProperties(const QmlObjectNode &qmlObjectNode,
                                               PropertyNameView propertyName)
{
    const QVariant value = qmlObjectNode.modelValue("Layout."_sv + propertyName);
    QVariant marginsValue = qmlObjectNode.modelValue("Layout.margins");

    if (!marginsValue.isValid())
        marginsValue.setValue(0.0);

    if (value.isValid())
        return value;

    if ("fillHeight" == propertyName || "fillWidth" == propertyName)
        return false;

      if ("minimumWidth" == propertyName || "minimumHeight" == propertyName)
          return 0;

      if ("preferredWidth" == propertyName || "preferredHeight" == propertyName)
          return -1;

      if ("maximumWidth" == propertyName || "maximumHeight" == propertyName)
          return 0xffff;

     if ("columnSpan" == propertyName || "rowSpan" == propertyName)
         return 1;

     if ("topMargin" == propertyName || "bottomMargin" == propertyName ||
         "leftMargin" == propertyName || "rightMargin" == propertyName ||
         "margins" == propertyName)
         return marginsValue;

    return QVariant();
}

QVariant properDefaultInsightAttachedProperties(const QmlObjectNode &qmlObjectNode,
                                                PropertyNameView propertyName)
{
    const QVariant value = qmlObjectNode.modelValue("InsightCategory."_sv + propertyName);

    if (value.isValid())
        return value;

    return QString();
}
} // namespace

void PropertyEditorQmlBackend::setupLayoutAttachedProperties(const QmlObjectNode &qmlObjectNode, PropertyEditorView *propertyEditor)
{
    if (QmlItemNode(qmlObjectNode).isInLayout()) {

        static const PropertyNameList propertyNames =
            {"alignment", "column", "columnSpan", "fillHeight", "fillWidth", "maximumHeight", "maximumWidth",
                "minimumHeight", "minimumWidth", "preferredHeight", "preferredWidth", "row", "rowSpan",
                "topMargin", "bottomMargin", "leftMargin", "rightMargin", "margins"};

        for (PropertyNameView propertyName : propertyNames) {
            createPropertyEditorValue(qmlObjectNode,
                                      "Layout."_sv + propertyName,
                                      properDefaultLayoutAttachedProperties(qmlObjectNode,
                                                                            propertyName),
                                      propertyEditor);
        }
    }
}

void PropertyEditorQmlBackend::setupInsightAttachedProperties(const QmlObjectNode &qmlObjectNode,
                                                              PropertyEditorView *propertyEditor)
{
    const PropertyName propertyName = "category";
    createPropertyEditorValue(qmlObjectNode,
                              "InsightCategory."_sv + propertyName,
                              properDefaultInsightAttachedProperties(qmlObjectNode, propertyName),
                              propertyEditor);
}

void PropertyEditorQmlBackend::setupAuxiliaryProperties(const QmlObjectNode &qmlObjectNode,
                                                        PropertyEditorView *propertyEditor)
{
    const QmlItemNode itemNode(qmlObjectNode);

    auto createProperty = [&](auto &&...properties) {
        (createPropertyEditorValue(qmlObjectNode,
                                   auxNamePostFix(properties.name),
                                   properDefaultAuxiliaryProperties(qmlObjectNode, properties),
                                   propertyEditor),
         ...);
    };

    constexpr auto commonProperties = std::make_tuple(customIdProperty);

    if (itemNode.isFlowTransition()) {
        constexpr auto properties = std::make_tuple(colorProperty,
                                                    widthProperty,
                                                    inOffsetProperty,
                                                    dashProperty,
                                                    breakPointProperty,
                                                    typeProperty,
                                                    radiusProperty,
                                                    bezierProperty,
                                                    labelPositionProperty,
                                                    labelFlipSideProperty);
        std::apply(createProperty, std::tuple_cat(commonProperties, properties));
    } else if (itemNode.isFlowItem()) {
        constexpr auto properties = std::make_tuple(colorProperty,
                                                    widthProperty,
                                                    inOffsetProperty,
                                                    outOffsetProperty,
                                                    joinConnectionProperty);
        std::apply(createProperty, std::tuple_cat(commonProperties, properties));
    } else if (itemNode.isFlowActionArea()) {
        constexpr auto properties = std::make_tuple(colorProperty,
                                                    widthProperty,
                                                    fillColorProperty,
                                                    outOffsetProperty,
                                                    dashProperty);
        std::apply(createProperty, std::tuple_cat(commonProperties, properties));
    } else if (itemNode.isFlowDecision()) {
        constexpr auto properties = std::make_tuple(colorProperty,
                                                    widthProperty,
                                                    fillColorProperty,
                                                    dashProperty,
                                                    blockSizeProperty,
                                                    blockRadiusProperty,
                                                    showDialogLabelProperty,
                                                    dialogLabelPositionProperty);
        std::apply(createProperty, std::tuple_cat(commonProperties, properties));
    } else if (itemNode.isFlowWildcard()) {
        constexpr auto properties = std::make_tuple(colorProperty,
                                                    widthProperty,
                                                    fillColorProperty,
                                                    dashProperty,
                                                    blockSizeProperty,
                                                    blockRadiusProperty);
        std::apply(createProperty, std::tuple_cat(commonProperties, properties));
    } else if (itemNode.isFlowView()) {
        constexpr auto properties = std::make_tuple(transitionColorProperty,
                                                    areaColorProperty,
                                                    areaFillColorProperty,
                                                    blockColorProperty,
                                                    transitionTypeProperty,
                                                    transitionRadiusProperty,
                                                    transitionBezierProperty);
        std::apply(createProperty, std::tuple_cat(commonProperties, properties));
    } else {
        std::apply(createProperty, commonProperties);
    }
}

void PropertyEditorQmlBackend::handleInstancePropertyChangedInModelNodeProxy(
    const ModelNode &modelNode, PropertyNameView propertyName)
{
    m_backendModelNode.handleInstancePropertyChanged(modelNode, propertyName);
}

void PropertyEditorQmlBackend::handleAuxiliaryDataChanges(const QmlObjectNode &qmlObjectNode,
                                                          AuxiliaryDataKeyView key)
{
    if (qmlObjectNode.isRootModelNode() && isMaterialAuxiliaryKey(key)) {
        m_backendMaterialNode.handleAuxiliaryPropertyChanges();
        m_view->instanceImageProvider()->invalidate();
    }
}

void PropertyEditorQmlBackend::handleVariantPropertyChangedInModelNodeProxy(const VariantProperty &property)
{
    m_backendModelNode.handleVariantPropertyChanged(property);
    updateInstanceImage();
}

void PropertyEditorQmlBackend::handleBindingPropertyChangedInModelNodeProxy(const BindingProperty &property)
{
    m_backendModelNode.handleBindingPropertyChanged(property);
    m_backendTextureNode.handleBindingPropertyChanged(property);
    updateInstanceImage();
}

void PropertyEditorQmlBackend::handleBindingPropertyInModelNodeProxyAboutToChange(
    const BindingProperty &property)
{
    if (m_backendMaterialNode.materialNode()) {
        ModelNode expressionNode = property.resolveToModelNode();
        if (expressionNode.metaInfo().isQtQuick3DTexture())
            updateInstanceImage();
    }
    m_backendTextureNode.handleBindingPropertyChanged(property);
}

void PropertyEditorQmlBackend::handlePropertiesRemovedInModelNodeProxy(const AbstractProperty &property)
{
    m_backendModelNode.handlePropertiesRemoved(property);
    m_backendTextureNode.handlePropertiesRemoved(property);
    updateInstanceImage();
}

void PropertyEditorQmlBackend::handleModelNodePreviewPixmapChanged(const ModelNode &node,
                                                                   const QPixmap &pixmap,
                                                                   const QByteArray &requestId)
{
    InstanceImageProvider *imageProvider = m_view->instanceImageProvider();

    if (!imageProvider)
        return;

    bool imageFed = imageProvider->feedImage(node, pixmap, requestId);
    if (imageFed && !imageProvider->hasPendingRequest())
        refreshPreview();
}

void PropertyEditorQmlBackend::handleModelSelectedNodesChanged(PropertyEditorView *propertyEditor)
{
    contextObject()->setHas3DModelSelected(!Utils3D::getSelectedModels(propertyEditor).isEmpty());
    m_backendTextureNode.updateSelectionDetails();
}

void PropertyEditorQmlBackend::createPropertyEditorValue(const QmlObjectNode &qmlObjectNode,
                                                         PropertyNameView name,
                                                         const QVariant &value,
                                                         PropertyEditorView *propertyEditor)
{
    PropertyName propertyName(name.toByteArray());
    propertyName.replace('.', '_');
    auto valueObject = qobject_cast<PropertyEditorValue*>(variantToQObject(backendValuesPropertyMap().value(QString::fromUtf8(propertyName))));
    if (!valueObject) {
        valueObject = new PropertyEditorValue(&backendValuesPropertyMap());
        QObject::connect(valueObject, &PropertyEditorValue::valueChanged, &backendValuesPropertyMap(), &DesignerPropertyMap::valueChanged);
        QObject::connect(valueObject, &PropertyEditorValue::expressionChanged, propertyEditor, &PropertyEditorView::changeExpression);
        QObject::connect(valueObject, &PropertyEditorValue::exportPropertyAsAliasRequested, propertyEditor, &PropertyEditorView::exportPropertyAsAlias);
        QObject::connect(valueObject, &PropertyEditorValue::removeAliasExportRequested, propertyEditor, &PropertyEditorView::removeAliasExport);
        backendValuesPropertyMap().insert(QString::fromUtf8(propertyName), QVariant::fromValue(valueObject));
    }
    valueObject->setName(name);
    valueObject->setModelNode(qmlObjectNode);

    if (qmlObjectNode.propertyAffectedByCurrentState(name)
        && !(qmlObjectNode.hasBindingProperty(name)))
        valueObject->setValue(qmlObjectNode.modelValue(name));

    else
        valueObject->setValue(value);

    if (propertyName != "id" &&
        qmlObjectNode.currentState().isBaseState() &&
        qmlObjectNode.modelNode().property(propertyName).isBindingProperty()) {
        valueObject->setExpression(qmlObjectNode.modelNode().bindingProperty(propertyName).expression());
    } else {
        if (qmlObjectNode.hasBindingProperty(name))
            valueObject->setExpression(qmlObjectNode.expression(name));
        else
            valueObject->setExpression(qmlObjectNode.instanceValue(name).toString());
    }
}

void PropertyEditorQmlBackend::setValue(const QmlObjectNode &,
                                        PropertyNameView name,
                                        const QVariant &value)
{
    // Vector*D values need to be split into their subcomponents
    if (value.typeId() == QMetaType::QVector2D) {
        const char *suffix[2] = {"_x", "_y"};
        auto vecValue = value.value<QVector2D>();
        for (int i = 0; i < 2; ++i) {
            PropertyName subPropName(name.size() + 2, '\0');
            subPropName.replace(0, name.size(), name);
            subPropName.replace(name.size(), 2, suffix[i]);
            auto propertyValue = qobject_cast<PropertyEditorValue *>(variantToQObject(m_backendValuesPropertyMap.value(QString::fromUtf8(subPropName))));
            if (propertyValue)
                propertyValue->setValue(QVariant(vecValue[i]));
        }
    } else if (value.typeId() == QMetaType::QVector3D) {
        const char *suffix[3] = {"_x", "_y", "_z"};
        auto vecValue = value.value<QVector3D>();
        for (int i = 0; i < 3; ++i) {
            PropertyName subPropName(name.size() + 2, '\0');
            subPropName.replace(0, name.size(), name);
            subPropName.replace(name.size(), 2, suffix[i]);
            auto propertyValue = qobject_cast<PropertyEditorValue *>(variantToQObject(m_backendValuesPropertyMap.value(QString::fromUtf8(subPropName))));
            if (propertyValue)
                propertyValue->setValue(QVariant(vecValue[i]));
        }
    } else if (value.typeId() == QMetaType::QVector4D) {
        const char *suffix[4] = {"_x", "_y", "_z", "_w"};
        auto vecValue = value.value<QVector4D>();
        for (int i = 0; i < 4; ++i) {
            PropertyName subPropName(name.size() + 2, '\0');
            subPropName.replace(0, name.size(), name);
            subPropName.replace(name.size(), 2, suffix[i]);
            auto propertyValue = qobject_cast<PropertyEditorValue *>(
                variantToQObject(m_backendValuesPropertyMap.value(QString::fromUtf8(subPropName))));
            if (propertyValue)
                propertyValue->setValue(QVariant(vecValue[i]));
        }
    } else {
        PropertyName propertyName = name.toByteArray();
        propertyName.replace('.', '_');
        auto propertyValue = qobject_cast<PropertyEditorValue *>(variantToQObject(m_backendValuesPropertyMap.value(QString::fromUtf8(propertyName))));
        if (propertyValue)
            propertyValue->setValue(value);
    }
}

void PropertyEditorQmlBackend::setExpression(PropertyNameView propName, const QString &exp)
{
    PropertyEditorValue *propertyValue = propertyValueForName(QString::fromUtf8(propName));
    if (propertyValue)
        propertyValue->setExpression(exp);
}

QQmlContext *PropertyEditorQmlBackend::context()
{
    return m_view->rootContext();
}

PropertyEditorContextObject *PropertyEditorQmlBackend::contextObject()
{
    return m_contextObject.get();
}

QQuickWidget *PropertyEditorQmlBackend::widget()
{
    return m_view.get();
}

void PropertyEditorQmlBackend::setSource(const QUrl &url)
{
    m_view->setSource(url);

    const bool showError = qEnvironmentVariableIsSet(Constants::ENVIRONMENT_SHOW_QML_ERRORS);

    if (showError && !m_view->errors().isEmpty()) {
        const QString errMsg = m_view->errors().constFirst().toString();
        Core::AsynchronousMessageBox::warning(Tr::tr("Invalid QML Source"), errMsg);
    }
}

QmlAnchorBindingProxy &PropertyEditorQmlBackend::backendAnchorBinding()
{
    return m_backendAnchorBinding;
}

DesignerPropertyMap &PropertyEditorQmlBackend::backendValuesPropertyMap() {
    return m_backendValuesPropertyMap;
}

PropertyEditorTransaction *PropertyEditorQmlBackend::propertyEditorTransaction() {
    return m_propertyEditorTransaction.get();
}

PropertyEditorValue *PropertyEditorQmlBackend::propertyValueForName(const QString &propertyName)
{
     return qobject_cast<PropertyEditorValue*>(variantToQObject(backendValuesPropertyMap().value(propertyName)));
}

void QmlDesigner::PropertyEditorQmlBackend::createPropertyEditorValues(const QmlObjectNode &qmlObjectNode, PropertyEditorView *propertyEditor)
{
#ifndef QDS_USE_PROJECTSTORAGE
    for (const auto &property : PropertyEditorUtils::filteredProperties(qmlObjectNode.metaInfo())) {
        auto propertyName = property.name();
        createPropertyEditorValue(qmlObjectNode,
                                  propertyName,
                                  qmlObjectNode.instanceValue(propertyName),
                                  propertyEditor);
    }
#else

    for (const auto &property : MetaInfoUtils::addInflatedValueAndReadOnlyProperties(qmlObjectNode.metaInfo().properties())) {
        auto propertyName = property.name();
        createPropertyEditorValue(qmlObjectNode,
                                  propertyName,
                                  qmlObjectNode.instanceValue(propertyName),
                                  propertyEditor);
    }
#endif
}

PropertyEditorValue *PropertyEditorQmlBackend::insertValue(const QString &name,
                                                           const QVariant &value,
                                                           const ModelNode &modelNode)
{
    auto valueObject = qobject_cast<PropertyEditorValue *>(
        variantToQObject(m_backendValuesPropertyMap.value(name)));
    if (!valueObject)
        valueObject = new PropertyEditorValue(&m_backendValuesPropertyMap);
    valueObject->setName(name.toLatin1());

    if (modelNode)
        valueObject->setModelNode(modelNode);

    if (value.isValid())
        valueObject->setValue(value);

    QObject::connect(valueObject,
                     &PropertyEditorValue::valueChanged,
                     &backendValuesPropertyMap(),
                     &DesignerPropertyMap::valueChanged);
    m_backendValuesPropertyMap.insert(name, QVariant::fromValue(valueObject));

    return valueObject;
}

void PropertyEditorQmlBackend::updateInstanceImage()
{
    m_view->instanceImageProvider()->invalidate();
    refreshPreview();
}

void PropertyEditorQmlBackend::setup(const ModelNodes &editorNodes,
                                     const QString &stateName,
                                     const QUrl &qmlSpecificsFile,
                                     PropertyEditorView *propertyEditor)
{
    QmlObjectNode qmlObjectNode(editorNodes.isEmpty() ? ModelNode{} : editorNodes.first());
    if (!qmlObjectNode.isValid()) {
        qWarning() << "PropertyEditor: invalid node for setup";
        return;
    }

    m_contextObject->setModel(propertyEditor->model());

    qCInfo(propertyEditorBenchmark) << Q_FUNC_INFO;

    QElapsedTimer time;
    if (propertyEditorBenchmark().isInfoEnabled())
        time.start();

    createPropertyEditorValues(qmlObjectNode, propertyEditor);
    setupLayoutAttachedProperties(qmlObjectNode, propertyEditor);
    setupInsightAttachedProperties(qmlObjectNode, propertyEditor);
    setupAuxiliaryProperties(qmlObjectNode, propertyEditor);

    // model node
    m_backendModelNode.setup(editorNodes);
    context()->setContextProperty("modelNodeBackend", &m_backendModelNode);

    m_backendMaterialNode.setup(editorNodes);
    m_backendTextureNode.setup(qmlObjectNode);

    insertValue(Constants::PROPERTY_EDITOR_CLASSNAME_PROPERTY,
                m_backendModelNode.simplifiedTypeName(),
                qmlObjectNode.modelNode());

    insertValue("id"_L1, m_backendModelNode.nodeId());
    insertValue("objectName"_L1, m_backendModelNode.nodeObjectName());

    QmlItemNode itemNode(qmlObjectNode.modelNode());

    // anchors
    m_backendAnchorBinding.setup(qmlObjectNode.modelNode());
    setupContextProperties();

    contextObject()->setHasMultiSelection(m_backendModelNode.multiSelection());

    qCInfo(propertyEditorBenchmark) << "anchors:" << time.elapsed();

    qCInfo(propertyEditorBenchmark) << "context:" << time.elapsed();

    contextObject()->setSpecificsUrl(qmlSpecificsFile);

    qCInfo(propertyEditorBenchmark) << "specifics:" << time.elapsed();

    contextObject()->setStateName(stateName);

    context()->setContextProperty(QLatin1String("propertyCount"),
                                  QVariant(qmlObjectNode.modelNode().properties().size()));

    QStringList stateNames = qmlObjectNode.allStateNames();
    stateNames.prepend("base state");
    contextObject()->setAllStateNames(stateNames);

    contextObject()->setIsBaseState(qmlObjectNode.isInBaseState());

    contextObject()->setHasAliasExport(qmlObjectNode.isAliasExported());

    contextObject()->setHasActiveTimeline(QmlTimeline::hasActiveTimeline(qmlObjectNode.view()));

    contextObject()->setSelectionChanged(false);

    NodeMetaInfo metaInfo = qmlObjectNode.modelNode().metaInfo();

#ifdef QDS_USE_PROJECTSTORAGE
    contextObject()->setMajorVersion(-1);
    contextObject()->setMinorVersion(-1);

    const auto version = QmlProjectManager::QmlProject::qtQuickVersion();
    contextObject()->setMajorQtQuickVersion(version.major);
    contextObject()->setMinorQtQuickVersion(version.minor);
#else
    if (metaInfo.isValid()) {
        contextObject()->setMajorVersion(metaInfo.majorVersion());
        contextObject()->setMinorVersion(metaInfo.minorVersion());
    } else {
        contextObject()->setMajorVersion(-1);
        contextObject()->setMinorVersion(-1);
    }
    contextObject()->setMajorQtQuickVersion(qmlObjectNode.view()->majorQtQuickVersion());
    contextObject()->setMinorQtQuickVersion(qmlObjectNode.view()->minorQtQuickVersion());
#endif
    contextObject()->setHasMaterialLibrary(Utils3D::materialLibraryNode(propertyEditor).isValid());
    contextObject()->setIsQt6Project(propertyEditor->externalDependencies().isQt6Project());
    contextObject()->setEditorNodes(editorNodes);
    contextObject()->setHasQuick3DImport(propertyEditor->model()->hasImport("QtQuick3D"));

    m_view->instanceImageProvider()->setModelNode(m_backendModelNode.singleSelectedNode());
    updateInstanceImage();

    qCInfo(propertyEditorBenchmark) << "final:" << time.elapsed();
}

QString PropertyEditorQmlBackend::propertyEditorResourcesPath()
{
    return resourcesPath("propertyEditorQmlSources");
}

QString PropertyEditorQmlBackend::scriptsEditorResourcesPath()
{
    return resourcesPath("scriptseditor");
}

inline bool dotPropertyHeuristic(const QmlObjectNode &node,
                                 const NodeMetaInfo &type,
                                 PropertyNameView name)
{
    if (!name.contains("."))
        return true;

    if (name.count('.') > 1)
        return false;

    QList<QByteArray> list = name.toByteArray().split('.');
    const PropertyName parentProperty = list.first();
    const PropertyName itemProperty = list.last();

    NodeMetaInfo propertyType = type.property(parentProperty).propertyType();

    NodeMetaInfo itemInfo = node.view()->model()->qtQuickItemMetaInfo();
    NodeMetaInfo textInfo = node.view()->model()->qtQuickTextMetaInfo();
    NodeMetaInfo rectangleInfo = node.view()->model()->qtQuickRectangleMetaInfo();
    NodeMetaInfo imageInfo = node.view()->model()->qtQuickImageMetaInfo();
    NodeMetaInfo fontInfo = node.view()->model()->fontMetaInfo();
    NodeMetaInfo vector4dInfo = node.view()->model()->vector4dMetaInfo();
    NodeMetaInfo textureInfo = node.view()->model()->qtQuick3DTextureMetaInfo();

    if (itemInfo.hasProperty(itemProperty)
        || propertyType.isBasedOn(textInfo, fontInfo, rectangleInfo, imageInfo, vector4dInfo, textureInfo))
        return false;

    return true;
}

#ifndef QDS_USE_PROJECTSTORAGE
QString PropertyEditorQmlBackend::templateGeneration(const NodeMetaInfo &metaType,
                                                     const NodeMetaInfo &superType,
                                                     const QmlObjectNode &node)
{
    // If we have dynamically generated specifics file for the type, prefer using it
    QUrl dynamicUrl = PropertyEditorQmlBackend::getQmlFileUrl(
        metaType.typeName() + "SpecificsDynamic", metaType);

    if (checkIfUrlExists(dynamicUrl)) {
        Utils::FilePath fp = Utils::FilePath::fromString(fileFromUrl(dynamicUrl));
        return QString::fromUtf8(fp.fileContents().value_or(QByteArray()));
    }

    if (!templateConfiguration() || !templateConfiguration()->isValid())
        return QString();

    const auto nodes = templateConfiguration()->children();

    QStringList allTypes; // all template types
    QStringList separateSectionTypes; // separate section types only
    QStringList needsTypeArgTypes;  // types that need type as third parameter

    for (const QmlJS::SimpleReaderNode::Ptr &node : nodes) {
        if (node->propertyNames().contains("separateSection"))
            separateSectionTypes.append(variantToStringList(node->property("typeNames").value));
        if (node->propertyNames().contains("needsTypeArg"))
            needsTypeArgTypes.append(variantToStringList(node->property("typeNames").value));

        allTypes.append(variantToStringList(node->property("typeNames").value));
    }

    auto propertyMetaInfoCompare = [](const auto &first, const auto &second) {
        return first.name() < second.name();
    };
    std::map<PropertyMetaInfo, PropertyMetaInfos, decltype(propertyMetaInfoCompare)> propertyMap(
        propertyMetaInfoCompare);
    PropertyMetaInfos separateSectionProperties;

    // Iterate over all properties and isolate the properties which have their own template
    for (const auto &property : PropertyEditorUtils::filteredProperties(metaType)) {
        const auto &propertyName = property.name();
        if (propertyName.startsWith("__"))
            continue; // private API

        if (!superType.hasProperty(propertyName) // TODO add property.isLocalProperty()
            && property.isWritable() && dotPropertyHeuristic(node, metaType, propertyName)) {
            QString typeName = QString::fromUtf8(property.propertyType().simplifiedTypeName());

            if (typeName == "alias" && node.isValid())
                typeName = QString::fromUtf8(node.instanceType(propertyName));

            // Check if a template for the type exists
            if (allTypes.contains(typeName)) {
                if (separateSectionTypes.contains(typeName)) { // template enforces separate section
                    separateSectionProperties.push_back(property);
                } else {
                    if (propertyName.contains('.')) {
                        const PropertyName parentPropertyName = propertyName.split('.').first();
                        const PropertyMetaInfo parentProperty = metaType.property(parentPropertyName);

                        auto vectorFound = std::find(separateSectionProperties.begin(),
                                                     separateSectionProperties.end(),
                                                     parentProperty);

                        auto propertyMapFound = propertyMap.find(parentProperty);

                        const bool exists = propertyMapFound != propertyMap.end()
                                            || vectorFound != separateSectionProperties.end();

                        if (!exists)
                            propertyMap[parentProperty].push_back(property);
                    } else {
                        propertyMap[property];
                    }
                }
            }
        }
    }

    // Filter out the properties which have a basic type e.g. int, string, bool
    PropertyMetaInfos basicProperties;
    auto it = propertyMap.begin();
    while (it != propertyMap.end()) {
        if (it->second.empty()) {
            basicProperties.push_back(it->first);
            it = propertyMap.erase(it);
        } else {
            ++it;
        }
    }

    Utils::sort(basicProperties, propertyMetaInfoCompare);

    auto findAndFillTemplate = [&nodes, &node, &needsTypeArgTypes](PropertyNameView label,
                                                                   const PropertyMetaInfo &property) {
        const auto &propertyName = property.name();
        PropertyName underscoreProperty = propertyName;
        underscoreProperty.replace('.', '_');

        TypeName typeName = property.propertyType().simplifiedTypeName();
        // alias resolution only possible with instance
        if (!useProjectStorage() && typeName == "alias" && node.isValid())
            typeName = node.instanceType(propertyName);

        QString filledTemplate;
        for (const QmlJS::SimpleReaderNode::Ptr &n : nodes) {
            // Check if we have a template for the type
            if (variantToStringList(n->property(QStringLiteral("typeNames")).value).contains(QString::fromLatin1(typeName))) {
                const QString fileName = propertyTemplatesPath() + n->property(QStringLiteral("sourceFile")).value.toString();
                QFile file(fileName);
                if (file.open(QIODevice::ReadOnly)) {
                    QString source = QString::fromUtf8(file.readAll());
                    file.close();
                    if (needsTypeArgTypes.contains(QString::fromUtf8(typeName))) {
                        filledTemplate = source.arg(QString::fromUtf8(label),
                                                    QString::fromUtf8(underscoreProperty),
                                                    QString::fromUtf8(typeName));
                    } else {
                        filledTemplate = source.arg(QString::fromUtf8(label),
                                                    QString::fromUtf8(underscoreProperty));
                    }
                } else {
                    qWarning().nospace() << "template definition source file not found:" << fileName;
                }
            }
        }
        return filledTemplate;
    };

    // QML specfics preparation
    QStringList imports = variantToStringList(templateConfiguration()->property(QStringLiteral("imports")).value);
    QString qmlTemplate = imports.join(QLatin1Char('\n')) + QLatin1Char('\n');
    bool emptyTemplate = true;

    const QString anchorLeftRight = "anchors.left: parent.left\nanchors.right: parent.right\n";

    qmlTemplate += "Column {\n";
    qmlTemplate += "width: parent.width\n";

    bool isEditableComponent = node.modelNode().isComponent() && !QmlItemNode(node).isEffectItem();
    if (isEditableComponent)
        qmlTemplate += "ComponentButton {}\n";

    QString qmlInnerTemplate = "";

    qmlInnerTemplate += "Section {\n";
    qmlInnerTemplate += "caption: \"" + Tr::tr("Exposed Custom Properties") + "\"\n";
    qmlInnerTemplate += anchorLeftRight;
    qmlInnerTemplate += "leftPadding: 0\n";
    qmlInnerTemplate += "rightPadding: 0\n";
    qmlInnerTemplate += "bottomPadding: 0\n";
    qmlInnerTemplate += "Column {\n";
    qmlInnerTemplate += "width: parent.width\n";

    // First the section containing properties of basic type e.g. int, string, bool
    if (!basicProperties.empty()) {
        emptyTemplate = false;

        qmlInnerTemplate += "Column {\n";
        qmlInnerTemplate += "width: parent.width\n";
        qmlInnerTemplate += "leftPadding: 8\n";
        qmlInnerTemplate += "bottomPadding: 10\n";
        qmlInnerTemplate += "SectionLayout {\n";

        for (const auto &basicProperty : std::as_const(basicProperties))
            qmlInnerTemplate += findAndFillTemplate(basicProperty.name(), basicProperty);

        qmlInnerTemplate += "}\n"; // SectionLayout
        qmlInnerTemplate += "}\n"; // Column
    }

    // Second the section containing properties of complex type for which no specific template exists e.g. Button
    if (!propertyMap.empty()) {
        emptyTemplate = false;
        for (auto &[property, properties] : propertyMap) {
            //     for (auto it = propertyMap.cbegin(); it != propertyMap.cend(); ++it) {
            TypeName parentTypeName = property.propertyType().simplifiedTypeName();
            // alias resolution only possible with instance
            if (!useProjectStorage() && parentTypeName == "alias" && node.isValid())
                parentTypeName = node.instanceType(property.name());

            qmlInnerTemplate += "Section {\n";
            qmlInnerTemplate += QStringLiteral("caption: \"%1 - %2\"\n")
                                    .arg(QString::fromUtf8(property.name()),
                                         QString::fromUtf8(parentTypeName));
            qmlInnerTemplate += anchorLeftRight;
            qmlInnerTemplate += "leftPadding: 8\n";
            qmlInnerTemplate += "rightPadding: 0\n";
            qmlInnerTemplate += "expanded: false\n";
            qmlInnerTemplate += "level: 1\n";
            qmlInnerTemplate += "SectionLayout {\n";

            Utils::sort(properties, propertyMetaInfoCompare);

            for (const auto &subProperty : properties) {
                const auto &propertyName = subProperty.name();
                auto found = std::find(propertyName.rbegin(), propertyName.rend(), '.');
                const PropertyName shortName{found.base(),
                                             std::distance(found.base(), propertyName.end())};
                qmlInnerTemplate += findAndFillTemplate(shortName, property);
            }

            qmlInnerTemplate += "}\n"; // SectionLayout
            qmlInnerTemplate += "}\n"; // Section
        }
    }

    // Third the section containing properties of complex type for which a specific template exists e.g. Rectangle, Image
    if (!separateSectionProperties.empty()) {
        emptyTemplate = false;
        Utils::sort(separateSectionProperties, propertyMetaInfoCompare);
        for (const auto &property : separateSectionProperties)
            qmlInnerTemplate += findAndFillTemplate(property.name(), property);
    }

    qmlInnerTemplate += "}\n"; // Column
    qmlInnerTemplate += "}\n"; // Section

    if (!emptyTemplate)
        qmlTemplate += qmlInnerTemplate;

    qmlTemplate += "}\n"; // Column

    return qmlTemplate;
}

QUrl PropertyEditorQmlBackend::getQmlFileUrl(const TypeName &relativeTypeName, const NodeMetaInfo &info)
{
    return fileToUrl(locateQmlFile(info, QString::fromUtf8(fixTypeNameForPanes(relativeTypeName) + ".qml")));
}
#endif // QDS_USE_PROJECTSTORAGE

TypeName PropertyEditorQmlBackend::fixTypeNameForPanes(const TypeName &typeName)
{
    TypeName fixedTypeName = typeName;
    fixedTypeName.replace('.', '/');
    return fixedTypeName;
}

QString PropertyEditorQmlBackend::resourcesPath(const QString &dir)
{
#ifdef SHARE_QML_PATH
    if (Utils::qtcEnvironmentVariableIsSet("LOAD_QML_FROM_SOURCE"))
        return QLatin1String(SHARE_QML_PATH) + "/" + dir;
#endif
    return Core::ICore::resourcePath("qmldesigner/" + dir).toUrlishString();
}

void PropertyEditorQmlBackend::refreshBackendModel()
{
    m_backendModelNode.refresh();
}

void PropertyEditorQmlBackend::refreshPreview()
{
    auto qmlPreview = widget()->rootObject();

    if (qmlPreview && qmlPreview->metaObject()->indexOfMethod("refreshPreview()") > -1)
        QMetaObject::invokeMethod(qmlPreview, "refreshPreview");
}

void PropertyEditorQmlBackend::setupContextProperties()
{
    context()->setContextProperties({
        {"modelNodeBackend", QVariant::fromValue(&m_backendModelNode)},
        {"materialNodeBackend", QVariant::fromValue(&m_backendMaterialNode)},
        {"textureNodeBackend", QVariant::fromValue(&m_backendTextureNode)},
        {"anchorBackend", QVariant::fromValue(&m_backendAnchorBinding)},
        {"transaction", QVariant::fromValue(m_propertyEditorTransaction.get())},
        {"dummyBackendValue", QVariant::fromValue(m_dummyPropertyEditorValue.get())},
    });
}

#ifndef QDS_USE_PROJECTSTORAGE
TypeName PropertyEditorQmlBackend::qmlFileName(const NodeMetaInfo &nodeInfo)
{
    const TypeName fixedTypeName = fixTypeNameForPanes(nodeInfo.typeName());
    return fixedTypeName + "Pane.qml";
}
#endif

QUrl PropertyEditorQmlBackend::fileToUrl(const QString &filePath)  {
    QUrl fileUrl;

    if (filePath.isEmpty())
        return fileUrl;

    if (filePath.startsWith(':')) {
        fileUrl.setScheme(QLatin1String("qrc"));
        QString path = filePath;
        path.remove(0, 1); // remove trailing ':'
        fileUrl.setPath(path);
    } else {
        fileUrl = QUrl::fromLocalFile(filePath);
    }

    return fileUrl;
}

QUrl PropertyEditorQmlBackend::emptyPaneUrl()
{
    return fileToUrl(QDir(propertyEditorResourcesPath()).filePath("QtQuick/emptyPane.qml"_L1));
}

QString PropertyEditorQmlBackend::fileFromUrl(const QUrl &url)
{
    if (url.scheme() == QStringLiteral("qrc")) {
        const QString &path = url.path();
        return QStringLiteral(":") + path;
    }

    return url.toLocalFile();
}

bool PropertyEditorQmlBackend::checkIfUrlExists(const QUrl &url)
{
    const QString &file = fileFromUrl(url);
    return !file.isEmpty() && QFileInfo::exists(file);
}

void PropertyEditorQmlBackend::emitSelectionToBeChanged()
{
    m_backendModelNode.emitSelectionToBeChanged();
}

void PropertyEditorQmlBackend::emitSelectionChanged()
{
    m_backendModelNode.emitSelectionChanged();
}

void PropertyEditorQmlBackend::setValueforLayoutAttachedProperties(const QmlObjectNode &qmlObjectNode,
                                                                   PropertyNameView name)
{
    PropertyName propertyName = name.toByteArray();
    propertyName.replace("Layout.", "");
    setValue(qmlObjectNode, name, properDefaultLayoutAttachedProperties(qmlObjectNode, propertyName));

    if (propertyName == "margins") {
        const QVariant marginsValue = properDefaultLayoutAttachedProperties(qmlObjectNode, "margins");
        setValue(qmlObjectNode, "Layout.topMargin", marginsValue);
        setValue(qmlObjectNode, "Layout.bottomMargin", marginsValue);
        setValue(qmlObjectNode, "Layout.leftMargin", marginsValue);
        setValue(qmlObjectNode, "Layout.rightMargin", marginsValue);
    }
}

void PropertyEditorQmlBackend::setValueforInsightAttachedProperties(const QmlObjectNode &qmlObjectNode,
                                                                    PropertyNameView name)
{
    PropertyName propertyName = name.toByteArray();
    propertyName.replace("InsightCategory.", "");
    setValue(qmlObjectNode, name, properDefaultInsightAttachedProperties(qmlObjectNode, propertyName));
}

void PropertyEditorQmlBackend::setValueforAuxiliaryProperties(const QmlObjectNode &qmlObjectNode,
                                                              AuxiliaryDataKeyView key)
{
    const PropertyName propertyName = auxNamePostFix(key.name);
    setValue(qmlObjectNode, propertyName, qmlObjectNode.modelNode().auxiliaryDataWithDefault(key));
}

#ifndef QDS_USE_PROJECTSTORAGE
std::tuple<QUrl, NodeMetaInfo> PropertyEditorQmlBackend::getQmlUrlForMetaInfo(const NodeMetaInfo &metaInfo)
{
    if (metaInfo.isValid()) {
        const NodeMetaInfos &hierarchy = metaInfo.selfAndPrototypes();
        for (const NodeMetaInfo &info : hierarchy) {
            QUrl fileUrl = fileToUrl(locateQmlFile(info, QString::fromUtf8(qmlFileName(info))));
            if (fileUrl.isValid())
                return {fileUrl, info};
        }
    }

    return {emptyPaneUrl(), {}};
}

QString PropertyEditorQmlBackend::locateQmlFile(const NodeMetaInfo &info, const QString &relativePath)
{
    static const QDir fileSystemDir(PropertyEditorQmlBackend::propertyEditorResourcesPath());

    constexpr QLatin1String qmlDesignerSubfolder{"/designer/"};
    const QDir resourcesDir(QStringLiteral(":/propertyEditorQmlSources"));
    const QDir importDir(info.importDirectoryPath() + qmlDesignerSubfolder);
    const QDir importDirVersion(info.importDirectoryPath() + QStringLiteral(".")
                                + QString::number(info.majorVersion()) + qmlDesignerSubfolder);

    const QString relativePathWithoutEnding = relativePath.left(relativePath.size() - 4);
    const QString relativePathWithVersion = QString("%1_%2_%3.qml").arg(relativePathWithoutEnding
        ).arg(info.majorVersion()).arg(info.minorVersion());

    //Check for qml files with versions first

    const QString withoutDir = relativePath.split(QStringLiteral("/")).constLast();

    int lastSlash = importDirVersion.absoluteFilePath(withoutDir).lastIndexOf("/");
    QString dirPath = importDirVersion.absoluteFilePath(withoutDir).left(lastSlash);

    if (importDirVersion.exists(withoutDir) && !dirPath.endsWith("QtQuick/Controls.2/designer") && !dirPath.endsWith("QtQuick/Controls/designer"))
        return importDirVersion.absoluteFilePath(withoutDir);

    const QString withoutDirWithVersion = relativePathWithVersion.split(QStringLiteral("/")).constLast();

    QStringList possiblePaths = {
        fileSystemDir.absoluteFilePath(relativePathWithVersion),
        resourcesDir.absoluteFilePath(relativePathWithVersion),
        fileSystemDir.absoluteFilePath(relativePath),
        resourcesDir.absoluteFilePath(relativePath)
    };

    if (!importDir.isEmpty())
        possiblePaths.append({
            importDir.absoluteFilePath(relativePathWithVersion),
            //Since we are in a subfolder of the import we do not require the directory
            importDir.absoluteFilePath(withoutDirWithVersion),
            importDir.absoluteFilePath(relativePath),
            //Since we are in a subfolder of the import we do not require the directory
            importDir.absoluteFilePath(withoutDir),
        });

    return Utils::findOrDefault(possiblePaths, [](const QString &possibleFilePath) {
        return QFileInfo::exists(possibleFilePath);
    });
}
#endif // QDS_USE_PROJECTSTORAGE

} //QmlDesigner
