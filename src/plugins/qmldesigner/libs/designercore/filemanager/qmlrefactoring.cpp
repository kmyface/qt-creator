// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmlrefactoring.h"

#include <QDebug>

#include "addarraymembervisitor.h"
#include "addobjectvisitor.h"
#include "addpropertyvisitor.h"
#include "changeimportsvisitor.h"
#include "changeobjecttypevisitor.h"
#include "changepropertyvisitor.h"
#include "moveobjectvisitor.h"
#include "moveobjectbeforeobjectvisitor.h"
#include "removepropertyvisitor.h"
#include "removeuiobjectmembervisitor.h"

using namespace QmlJS;
using namespace QmlDesigner;
using namespace QmlDesigner::Internal;

QmlRefactoring::QmlRefactoring(const Document::Ptr &doc,
                               TextModifier &modifier,
                               Utils::span<const PropertyNameView> propertyOrder)
    : qmlDocument(doc)
    , textModifier(&modifier)
    , m_propertyOrder(propertyOrder)
{
}

bool QmlRefactoring::reparseDocument()
{
    const QString newSource = textModifier->text();

//    qDebug() << "QmlRefactoring::reparseDocument() new QML source:" << newSource;

    Document::MutablePtr tmpDocument(Document::create("<ModelToTextMerger>", Dialect::Qml));
    tmpDocument->setSource(newSource);

    if (tmpDocument->parseQml()) {
        qmlDocument = tmpDocument;
        return true;
    } else {
        qWarning() << "*** Possible problem: QML file wasn't parsed correctly.";
        qDebug() << "*** QML text:" << textModifier->text();
        QString errorMessage = QStringLiteral("Parsing Error");
        if (!tmpDocument->diagnosticMessages().isEmpty())
            errorMessage = tmpDocument->diagnosticMessages().constFirst().message;

        qDebug() <<  "*** " << errorMessage;
        return false;
    }
}

bool QmlRefactoring::addImport(const Import &import)
{
    ChangeImportsVisitor visitor(*textModifier, qmlDocument->source());
    return visitor.add(qmlDocument->qmlProgram(), import);
}

bool QmlRefactoring::removeImport(const Import &import)
{
    ChangeImportsVisitor visitor(*textModifier, qmlDocument->source());
    return visitor.remove(qmlDocument->qmlProgram(), import);
}

bool QmlRefactoring::addToArrayMemberList(int parentLocation,
                                          PropertyNameView propertyName,
                                          const QString &content)
{
    if (parentLocation < 0)
        return false;

    AddArrayMemberVisitor visit(*textModifier, parentLocation, QString::fromUtf8(propertyName), content);
    visit.setConvertObjectBindingIntoArrayBinding(true);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::addToObjectMemberList(int parentLocation,
                                           std::optional<int> nodeLocation,
                                           const QString &content)
{
    if (parentLocation < 0)
        return false;

    AddObjectVisitor visit(*textModifier, parentLocation, nodeLocation, content, m_propertyOrder);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::addProperty(int parentLocation,
                                 PropertyNameView name,
                                 const QString &value,
                                 PropertyType propertyType,
                                 const TypeName &dynamicTypeName)
{
    if (parentLocation < 0)
        return true; /* Node is not in hierarchy, yet and operation can be ignored. */

    AddPropertyVisitor visit(*textModifier, parentLocation, name, value, propertyType, m_propertyOrder, dynamicTypeName);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::changeProperty(int parentLocation,
                                    PropertyNameView name,
                                    const QString &value,
                                    PropertyType propertyType)
{
    if (parentLocation < 0)
        return false;

    ChangePropertyVisitor visit(*textModifier,
                                parentLocation,
                                QString::fromUtf8(name),
                                value,
                                propertyType);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::changeObjectType(int nodeLocation, const QString &newType)
{
    if (nodeLocation < 0 || newType.isEmpty())
        return false;

    ChangeObjectTypeVisitor visit(*textModifier, nodeLocation, newType);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::moveObject(int objectLocation,
                                PropertyNameView targetPropertyName,
                                bool targetIsArrayBinding,
                                int targetParentObjectLocation)
{
    if (objectLocation < 0 || targetParentObjectLocation < 0)
        return false;

    MoveObjectVisitor visit(*textModifier, objectLocation, targetPropertyName, targetIsArrayBinding, (quint32) targetParentObjectLocation, m_propertyOrder);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::moveObjectBeforeObject(int movingObjectLocation, int beforeObjectLocation, bool inDefaultProperty)
{
    if (movingObjectLocation < 0 || beforeObjectLocation < -1)
        return false;

    if (beforeObjectLocation == -1) {
        MoveObjectBeforeObjectVisitor visit(*textModifier, movingObjectLocation, inDefaultProperty);
        return visit(qmlDocument->qmlProgram());
    } else {
        MoveObjectBeforeObjectVisitor visit(*textModifier, movingObjectLocation, beforeObjectLocation, inDefaultProperty);
        return visit(qmlDocument->qmlProgram());
    }
    return false;
}

bool QmlRefactoring::removeObject(int nodeLocation)
{
    if (nodeLocation < 0)
        return false;

    RemoveUIObjectMemberVisitor visit(*textModifier, nodeLocation);
    return visit(qmlDocument->qmlProgram());
}

bool QmlRefactoring::removeProperty(int parentLocation, PropertyNameView name)
{
    if (parentLocation < 0 || name.isEmpty())
        return false;

    RemovePropertyVisitor visit(*textModifier, parentLocation, QString::fromUtf8(name));
    return visit(qmlDocument->qmlProgram());
}
