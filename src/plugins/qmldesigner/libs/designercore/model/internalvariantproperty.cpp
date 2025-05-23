// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "internalvariantproperty.h"

namespace QmlDesigner {
namespace Internal {

InternalVariantProperty::InternalVariantProperty(PropertyNameView name, const InternalNodePointer &node)
    : InternalProperty(name, node, PropertyType::Variant)
{
}

const QVariant &InternalVariantProperty::value() const
{
    return m_value;
}

void InternalVariantProperty::setValue(const QVariant &value)
{
    traceToken.tick("value", keyValue("value", value));

    m_value = value;
}

void InternalVariantProperty::setDynamicValue(const TypeName &type, const QVariant &value)
{
     setValue(value);
     setDynamicTypeName(type);
}

bool InternalVariantProperty::isValid() const
{
    return InternalProperty::isValid() && isVariantProperty();
}

} // namespace Internal
} // namespace QmlDesigner
