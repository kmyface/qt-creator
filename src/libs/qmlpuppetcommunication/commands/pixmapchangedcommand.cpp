// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "pixmapchangedcommand.h"

#include <QDebug>

#include <QVarLengthArray>

#include <algorithm>

namespace QmlDesigner {

PixmapChangedCommand::PixmapChangedCommand() = default;

PixmapChangedCommand::PixmapChangedCommand(const QList<ImageContainer> &imageVector)
    : m_imageVector(imageVector)
{
}

QList<ImageContainer> PixmapChangedCommand::images() const
{
    return m_imageVector;
}

void PixmapChangedCommand::sort()
{
    std::sort(m_imageVector.begin(), m_imageVector.end());
}

QDataStream &operator<<(QDataStream &out, const PixmapChangedCommand &command)
{
    out << command.images();

    return out;
}

QDataStream &operator>>(QDataStream &in, PixmapChangedCommand &command)
{
    in >> command.m_imageVector;

    return in;
}

bool operator ==(const PixmapChangedCommand &first, const PixmapChangedCommand &second)
{
    return first.m_imageVector == second.m_imageVector;
}

QDebug operator <<(QDebug debug, const PixmapChangedCommand &command)
{
    return debug.nospace() << "PixmapChangedCommand(" << command.images() << ")";
}

} // namespace QmlDesigner
