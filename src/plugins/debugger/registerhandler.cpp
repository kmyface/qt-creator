// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "registerhandler.h"

#include "debuggerengine.h"

#include "memoryagent.h"
#include "debuggeractions.h"
#include "debuggerdialogs.h"
#include "debuggercore.h"
#include "debuggerengine.h"
#include "debuggertr.h"

#include <utils/basetreeview.h>
#include <utils/qtcassert.h>

#include <QDebug>
#include <QItemDelegate>
#include <QMenu>
#include <QPainter>

using namespace Utils;

namespace Debugger::Internal {

enum RegisterColumns
{
    RegisterNameColumn,
    RegisterValueColumn,
    RegisterColumnCount
};

enum RegisterDataRole
{
    RegisterChangedRole = Qt::UserRole
};

///////////////////////////////////////////////////////////////////////
//
// RegisterDelegate
//
///////////////////////////////////////////////////////////////////////

class RegisterDelegate : public QItemDelegate
{
public:
    RegisterDelegate(int column) : m_column(column) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &index) const override
    {
        if (index.column() == m_column) {
            auto lineEdit = new QLineEdit(parent);
            lineEdit->setAlignment(Qt::AlignLeft);
            lineEdit->setFrame(false);
            return lineEdit;
        }
        return nullptr;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        auto lineEdit = qobject_cast<QLineEdit *>(editor);
        QTC_ASSERT(lineEdit, return);
        lineEdit->setText(index.data(Qt::EditRole).toString());
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (index.column() == m_column) {
            auto lineEdit = qobject_cast<QLineEdit *>(editor);
            QTC_ASSERT(lineEdit, return);
            model->setData(index, lineEdit->text(), Qt::EditRole);
        }
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &) const override
    {
        editor->setGeometry(option.rect);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.column() == m_column) {
            const bool paintRed = index.data(Qt::UserRole).toBool();
            QPen oldPen = painter->pen();
            const QColor lightColor(140, 140, 140);
            if (paintRed)
                painter->setPen(QColor(200, 0, 0));
            else
                painter->setPen(lightColor);
            // FIXME: performance? this changes only on real font changes.
            QFontMetrics fm(option.font);
            int charWidth = qMax(fm.horizontalAdvance('x'), fm.horizontalAdvance('0'));
            QString str = index.data(Qt::DisplayRole).toString();
            int x = option.rect.x();
            bool light = !paintRed;
            for (int i = 0; i < str.size(); ++i) {
                const QChar c = str.at(i);
                const int uc = c.unicode();
                if (light && (uc != 'x' && uc != '0')) {
                    light = false;
                    painter->setPen(oldPen.color());
                }
                if (uc == ' ') {
                    light = true;
                    painter->setPen(lightColor);
                } else {
                    QRect r = option.rect;
                    r.setX(x);
                    r.setWidth(charWidth);
                    painter->drawText(r, Qt::AlignHCenter, c);
                }
                x += charWidth;
            }
            painter->setPen(oldPen);
        } else {
            QItemDelegate::paint(painter, option, index);
        }
    }

    const int m_column;
};

//////////////////////////////////////////////////////////////////
//
// Register
//
//////////////////////////////////////////////////////////////////

void Register::guessMissingData()
{
    if (reportedType == "int")
        kind = IntegerRegister;
    else if (reportedType == "float")
        kind = FloatRegister;
    else if (reportedType == "_i387_ext")
        kind = FloatRegister;
    else if (reportedType == "*1" || reportedType == "long")
        kind = IntegerRegister;
    else if (reportedType.contains("vec"))
        kind = VectorRegister;
    else if (reportedType.startsWith("int"))
        kind = IntegerRegister;
    else if (name.startsWith("xmm") || name.startsWith("ymm"))
        kind = VectorRegister;
}

static QString subTypeName(RegisterKind kind, int size, RegisterFormat format)
{
    QString name('[');

    switch (kind) {
        case IntegerRegister: name += 'i'; break;
        case FloatRegister: name += 'f'; break;
        default: break;
    }

    name += QString::number(size);

    switch (format) {
        case BinaryFormat: name += 'b'; break;
        case OctalFormat: name += 'o'; break;
        case DecimalFormat: name += 'u'; break;
        case SignedDecimalFormat: name += 's'; break;
        case HexadecimalFormat: name += 'x'; break;
        case CharacterFormat: name += 'c'; break;
    }

    name += ']';

    return name;
}

static uint decodeHexChar(unsigned char c)
{
    c -= '0';
    if (c < 10)
        return c;
    c -= 'A' - '0';
    if (c < 6)
        return 10 + c;
    c -= 'a' - 'A';
    if (c < 6)
        return 10 + c;
    return uint(-1);
}

void RegisterValue::fromString(const QString &str, RegisterFormat format)
{
    known = !str.isEmpty();
    v.u64[1] = v.u64[0] = 0;

    const int n = str.size();
    int pos = 0;
    if (str.startsWith("0x"))
        pos += 2;

    bool negative = pos < n && str.at(pos) == '-';
    if (negative)
        ++pos;

    while (pos < n) {
        uint c = str.at(pos).unicode();
        if (format != CharacterFormat) {
            c = decodeHexChar(c);
            if (c == uint(-1))
                break;
        }
        shiftOneDigit(c, format);
        ++pos;
    }

    if (negative) {
        v.u64[1] = ~v.u64[1];
        v.u64[0] = ~v.u64[0];
        ++v.u64[0];
        if (v.u64[0] == 0)
            ++v.u64[1];
    }
}

bool RegisterValue::operator==(const RegisterValue &other)
{
    return v.u64[0] == other.v.u64[0] && v.u64[1] == other.v.u64[1];
}

static QString formatRegister(quint64 v, int size, RegisterFormat format, bool forEdit)
{
    QString result;
    if (format == HexadecimalFormat) {
        result = QString::number(v, 16);
        result.prepend(QString(2*size - result.size(), '0'));
    } else if (format == DecimalFormat) {
        result = QString::number(v, 10);
        result.prepend(QString(2*size - result.size(), ' '));
    } else if (format == SignedDecimalFormat) {
        qint64 sv;
        if (size >= 8)
            sv = qint64(v);
        else if (size >= 4)
            sv = qint32(v);
        else if (size >= 2)
            sv = qint16(v);
        else
            sv = qint8(v);
        result = QString::number(sv, 10);
        result.prepend(QString(2*size - result.size(), ' '));
    } else if (format == CharacterFormat) {
        bool spacesOnly = true;
        if (v >= 32 && v < 127) {
            spacesOnly = false;
            if (!forEdit)
                result += '\'';
            result += char(v);
            if (!forEdit)
                result += '\'';
        } else {
            result += "   ";
        }
        if (spacesOnly && forEdit)
            result.clear();
        else
            result.prepend(QString(2*size - result.size(), ' '));
    }
    return result;
}

QString RegisterValue::toString(RegisterKind kind, int size, RegisterFormat format, bool forEdit) const
{
    if (!known)
        return QLatin1String("[inaccessible]");
    if (kind == FloatRegister) {
        if (size == 4)
            return QString::number(v.f[0]);
        if (size == 8)
            return QString::number(v.d[0]);
    }

    QString result;
    if (size > 8) {
        result += formatRegister(v.u64[1], size - 8, format, forEdit);
        size = 8;
        if (format != HexadecimalFormat)
            result += ',';
    }
    return result + formatRegister(v.u64[0], size, format, forEdit);
}

RegisterValue RegisterValue::subValue(int size, int index) const
{
    RegisterValue value;
    value.known = known;
    switch (size) {
        case 1:
            value.v.u8[0] = v.u8[index];
            break;
        case 2:
            value.v.u16[0] = v.u16[index];
            break;
        case 4:
            value.v.u32[0] = v.u32[index];
            break;
        case 8:
            value.v.u64[0] = v.u64[index];
            break;
    }
    return value;
}

void RegisterValue::setSubValue(int size, int index, RegisterValue subValue)
{
    switch (size) {
        case 1:
            v.u8[index] = subValue.v.u8[0];
            break;
        case 2:
            v.u16[index] = subValue.v.u16[0];
            break;
        case 4:
            v.u32[index] = subValue.v.u32[0];
            break;
        case 8:
            v.u64[index] = subValue.v.u64[0];
            break;
    }
}

static inline void shiftBitsLeft(RegisterValue *val, int amount)
{
    val->v.u64[1] <<= amount;
    val->v.u64[1] |= val->v.u64[0] >> (64 - amount);
    val->v.u64[0] <<= amount;
}

void RegisterValue::shiftOneDigit(uint digit, RegisterFormat format)
{
    switch (format) {
    case HexadecimalFormat:
        shiftBitsLeft(this, 4);
        v.u64[0] |= digit;
        break;
    case OctalFormat:
        shiftBitsLeft(this, 3);
        v.u64[0] |= digit;
        break;
    case BinaryFormat:
        shiftBitsLeft(this, 1);
        v.u64[0] |= digit;
        break;
    case DecimalFormat:
    case SignedDecimalFormat: {
        shiftBitsLeft(this, 1);
        quint64 tmp0 = v.u64[0];
        quint64 tmp1 = v.u64[1];
        shiftBitsLeft(this, 2);
        v.u64[1] += tmp1;
        v.u64[0] += tmp0;
        if (v.u64[0] < tmp0)
            ++v.u64[1];
        v.u64[0] += digit;
        if (v.u64[0] < digit)
            ++v.u64[1];
        break;
    }
    case CharacterFormat:
        shiftBitsLeft(this, 8);
        v.u64[0] |= digit;
    }
}

//////////////////////////////////////////////////////////////////
//
// RegisterSubItem and RegisterItem
//
//////////////////////////////////////////////////////////////////

class RegisterEditItem : public TypedTreeItem<TreeItem, RegisterSubItem>
{
public:
    RegisterEditItem(int pos, RegisterKind subKind, int subSize, RegisterFormat format)
        : m_index(pos), m_subKind(subKind), m_subSize(subSize), m_subFormat(format)
    {}

    QVariant data(int column, int role) const override;
    bool setData(int column, const QVariant &value, int role) override;
    Qt::ItemFlags flags(int column) const override;

    int m_index;
    RegisterKind m_subKind;
    int m_subSize;
    RegisterFormat m_subFormat;
};

class RegisterSubItem : public TypedTreeItem<RegisterEditItem, RegisterItem>
{
public:
    RegisterSubItem(RegisterKind subKind, int subSize, int count, RegisterFormat format)
        : m_subKind(subKind), m_subFormat(format), m_subSize(subSize), m_count(count), m_changed(false)
    {
        for (int i = 0; i != count; ++i)
            appendChild(new RegisterEditItem(i, subKind, subSize, format));
    }

    QVariant data(int column, int role) const override;

    Qt::ItemFlags flags(int column) const override
    {
        //return column == 1 ? Qt::ItemIsSelectable|Qt::ItemIsEnabled|Qt::ItemIsEditable
        //                   : Qt::ItemIsSelectable|Qt::ItemIsEnabled;
        Q_UNUSED(column)
        return Qt::ItemIsSelectable|Qt::ItemIsEnabled;
    }

    RegisterKind m_subKind;
    RegisterFormat m_subFormat;
    int m_subSize;
    int m_count;
    bool m_changed;
};

class RegisterItem : public TypedTreeItem<RegisterSubItem>
{
public:
    RegisterItem(DebuggerEngine *engine, const Register &reg);

    QVariant data(int column, int role) const override;
    bool setData(int column, const QVariant &value, int role) override;
    Qt::ItemFlags flags(int column) const override;

    quint64 addressValue() const;
    void triggerChange();

    DebuggerEngine *m_engine;
    Register m_reg;
    RegisterFormat m_format = HexadecimalFormat;
    bool m_changed = true;
};

RegisterItem::RegisterItem(DebuggerEngine *engine, const Register &reg)
    : m_engine(engine), m_reg(reg)
{
    if (m_reg.kind == UnknownRegister)
        m_reg.guessMissingData();

    if (m_reg.kind == IntegerRegister || m_reg.kind == VectorRegister) {
        if (m_reg.size <= 8) {
            appendChild(new RegisterSubItem(IntegerRegister, m_reg.size, 1, SignedDecimalFormat));
            appendChild(new RegisterSubItem(IntegerRegister, m_reg.size, 1, DecimalFormat));
        }
        for (int s = m_reg.size / 2; s; s = s / 2) {
            appendChild(new RegisterSubItem(IntegerRegister, s, m_reg.size / s, HexadecimalFormat));
            appendChild(new RegisterSubItem(IntegerRegister, s, m_reg.size / s, SignedDecimalFormat));
            appendChild(new RegisterSubItem(IntegerRegister, s, m_reg.size / s, DecimalFormat));
            if (s == 1)
                appendChild(new RegisterSubItem(IntegerRegister, s, m_reg.size / s, CharacterFormat));
        }
    }
    if (m_reg.kind == IntegerRegister || m_reg.kind == VectorRegister) {
        for (int s = m_reg.size; s >= 4; s = s / 2)
            appendChild(new RegisterSubItem(FloatRegister, s, m_reg.size / s, DecimalFormat));
    }
}

Qt::ItemFlags RegisterItem::flags(int column) const
{
    const Qt::ItemFlags notEditable = Qt::ItemIsSelectable|Qt::ItemIsEnabled;
    // Can edit registers if they are hex numbers and not arrays.
    if (column == 1) //  && IntegerWatchLineEdit::isUnsignedHexNumber(m_reg.display))
        return notEditable | Qt::ItemIsEditable;
    return notEditable;
}

quint64 RegisterItem::addressValue() const
{
    return m_reg.value.v.u64[0];
}

void RegisterItem::triggerChange()
{
    QString value = "0x" + m_reg.value.toString(m_reg.kind, m_reg.size, HexadecimalFormat);
    m_engine->setRegisterValue(m_reg.name, value);
}

QVariant RegisterItem::data(int column, int role) const
{
    switch (role) {
        case RegisterChangedRole:
            return m_changed;

        case Qt::DisplayRole:
            switch (column) {
                case RegisterNameColumn: {
                    QString res = m_reg.name;
                    if (!m_reg.description.isEmpty())
                        res += " (" + m_reg.description + ')';
                    return res;
                }
                case RegisterValueColumn: {
                    return m_reg.value.toString(m_reg.kind, m_reg.size, m_format);
                }
            }
            break;

        case Qt::ToolTipRole:
            return QString("Current Value: %1\nPreviousValue: %2")
                    .arg(m_reg.value.toString(m_reg.kind, m_reg.size, m_format))
                    .arg(m_reg.previousValue.toString(m_reg.kind, m_reg.size, m_format));

        case Qt::EditRole: // Edit: Unpadded for editing
            return m_reg.value.toString(m_reg.kind, m_reg.size, m_format);

        case Qt::TextAlignmentRole:
            return column == RegisterValueColumn ? QVariant(Qt::AlignRight) : QVariant();

        default:
            break;
    }
    return QVariant();
}

bool RegisterItem::setData(int column, const QVariant &value, int role)
{
    if (column == RegisterValueColumn && role == Qt::EditRole) {
        m_reg.value.fromString(value.toString(), m_format);
        triggerChange();
        return true;
    }
    return false;
}

QVariant RegisterSubItem::data(int column, int role) const
{
    switch (role) {
        case RegisterChangedRole:
            return m_changed;

        case Qt::DisplayRole:
            switch (column) {
                case RegisterNameColumn:
                    return subTypeName(m_subKind, m_subSize, m_subFormat);
                case RegisterValueColumn: {
                    QTC_ASSERT(parent(), return QVariant());
                    RegisterValue value = parent()->m_reg.value;
                    QString ba;
                    for (int i = 0; i != m_count; ++i) {
                        int tab = 5 * (i + 1) * m_subSize;
                        QString b = value.subValue(m_subSize, i).toString(m_subKind, m_subSize, m_subFormat);
                        ba += QString(tab - ba.size() - b.size(), ' ');
                        ba += b;
                    }
                    return ba;
                }
            }
            break;

        case Qt::ToolTipRole:
            if (m_subKind == IntegerRegister) {
                if (m_subFormat == CharacterFormat)
                    return Tr::tr("Content as ASCII Characters");
                if (m_subFormat == SignedDecimalFormat)
                    return Tr::tr("Content as %1-bit Signed Decimal Values").arg(8 * m_subSize);
                if (m_subFormat == DecimalFormat)
                    return Tr::tr("Content as %1-bit Unsigned Decimal Values").arg(8 * m_subSize);
                if (m_subFormat == HexadecimalFormat)
                    return Tr::tr("Content as %1-bit Hexadecimal Values").arg(8 * m_subSize);
                if (m_subFormat == OctalFormat)
                    return Tr::tr("Content as %1-bit Octal Values").arg(8 * m_subSize);
                if (m_subFormat == BinaryFormat)
                    return Tr::tr("Content as %1-bit Binary Values").arg(8 * m_subSize);
            }
            if (m_subKind == FloatRegister)
                return Tr::tr("Content as %1-bit Floating Point Values").arg(8 * m_subSize);

        default:
            break;
    }

    return QVariant();
}

//////////////////////////////////////////////////////////////////
//
// RegisterGroup
//
//////////////////////////////////////////////////////////////////

class RegisterGroup : public TypedTreeItem<RegisterItem>
{
public:
    RegisterGroup(DebuggerEngine *engine, const QString &group);

    QVariant data(int column, int role) const override;
    Qt::ItemFlags flags(int column) const override;
    bool updateRegister(const Register &reg);

    DebuggerEngine *m_engine;
    QString m_group;
    bool m_changed = true;
private:
    QHash<QString, RegisterItem *> m_registerByName;
};

RegisterGroup::RegisterGroup(DebuggerEngine *engine, const QString &group)
    : m_engine(engine)
    , m_group(group)
{}

QVariant RegisterGroup::data(int column, int role) const
{
    switch (role) {
    case RegisterChangedRole:
        return m_changed;

    case Qt::DisplayRole:
        if (column == 0)
            return m_group;
        break;

    case Qt::ToolTipRole:
        return Tr::tr("A group of registers.");

    default:
        break;
    }
    return {};
}

Qt::ItemFlags RegisterGroup::flags(int /*column*/) const
{
    return Qt::ItemIsSelectable|Qt::ItemIsEnabled;
}

bool RegisterGroup::updateRegister(const Register &r)
{
    RegisterItem *reg = m_registerByName.value(r.name, nullptr);
    if (!reg) {
        reg = new RegisterItem(m_engine, r);
        m_registerByName[r.name] = reg;
        appendChild(reg);
        return false;
    }

    if (r.size > 0)
        reg->m_reg.size = r.size;
    if (!r.description.isEmpty())
        reg->m_reg.description = r.description;
    if (reg->m_reg.value != r.value) {
        // Indicate red if values change, keep changed.
        reg->m_changed = true;
        reg->m_reg.previousValue = reg->m_reg.value;
        reg->m_reg.value = r.value;
    } else {
        reg->m_changed = false;
    }
    return reg->m_changed;
}


//////////////////////////////////////////////////////////////////
//
// RegisterHandler
//
//////////////////////////////////////////////////////////////////

RegisterHandler::RegisterHandler(DebuggerEngine *engine)
    : m_engine(engine)
{
    setObjectName("RegisterModel");
    setHeader({Tr::tr("Name"), Tr::tr("Value")});
}

void RegisterHandler::updateRegister(const Register &r)
{
    bool sort = false;
    bool changed = false;
    const QStringList groups = r.groups.isEmpty() ? QStringList{"all"} : r.groups;
    for (const QString &group : groups) {
        RegisterGroup *regGr = m_registerGroups.value(group, nullptr);
        if (!regGr) {
            sort = true;
            m_registerGroups[group] = regGr = new RegisterGroup{m_engine, group};
            rootItem()->appendChild(regGr);
        }
        changed |= regGr->updateRegister(r);
    }
    if (sort) {
        rootItem()->sortChildren([](const RegisterGroup *a, const RegisterGroup *b){
           return a->m_group < b->m_group;
        });
    }
    if (changed)
        emit registerChanged(r.name, r.value.v.u64[0]); // Notify attached memory views.
}

RegisterMap RegisterHandler::registerMap() const
{
    RegisterGroup *allRegs = allRegisters();
    if (!allRegs)
        return {};
    RegisterMap result;
    for (int i = 0, n = allRegs->childCount(); i != n; ++i) {
        RegisterItem *reg = allRegs->childAt(i);
        quint64 value = reg->addressValue();
        if (value)
            result.insert(value, reg->m_reg.name);
    }
    return result;
}

QVariant RegisterHandler::data(const QModelIndex &idx, int role) const
{
    if (role == BaseTreeView::ItemDelegateRole)
        return QVariant::fromValue(createRegisterDelegate(RegisterValueColumn));

    return RegisterModel::data(idx, role);
}

bool RegisterHandler::setData(const QModelIndex &idx, const QVariant &data, int role)
{
    if (role == BaseTreeView::ItemViewEventRole) {
        ItemViewEvent ev = data.value<ItemViewEvent>();
        if (ev.type() == QEvent::ContextMenu)
            return contextMenuEvent(ev);
    }

    return RegisterModel::setData(idx, data, role);
}

bool RegisterHandler::contextMenuEvent(const ItemViewEvent &ev)
{
    const DebuggerState state = m_engine->state();
    const bool actionsEnabled = m_engine->debuggerActionsEnabled();

    RegisterItem *registerItem = itemForIndexAtLevel<2>(ev.sourceModelIndex());
    RegisterSubItem *registerSubItem = itemForIndexAtLevel<3>(ev.sourceModelIndex());

    const quint64 address = registerItem ? registerItem->addressValue() : 0;
    const QString registerName = registerItem ? registerItem->m_reg.name : QString();

    auto menu = new QMenu;

    addAction(this, menu, Tr::tr("Reload Register Listing"),
              m_engine->hasCapability(RegisterCapability)
                && (state == InferiorStopOk || state == InferiorUnrunnable),
              [this] { m_engine->reloadRegisters(); });

    menu->addSeparator();

    addAction(this, menu, Tr::tr("Open Memory View at Value of Register %1 0x%2")
              .arg(registerName).arg(address, 0, 16),
              Tr::tr("Open Memory View at Value of Register"),
              address,
              [this, registerName, address] {
                    MemoryViewSetupData data;
                    data.startAddress = address;
                    data.registerName = registerName;
                    data.trackRegisters = true;
                    data.separateView = true;
                    m_engine->openMemoryView(data);
              });

    addAction(this, menu, Tr::tr("Open Memory Editor at 0x%1").arg(address, 0, 16),
              Tr::tr("Open Memory Editor"),
              address && actionsEnabled && m_engine->hasCapability(ShowMemoryCapability),
              [this, registerName, address] {
                    MemoryViewSetupData data;
                    data.startAddress = address;
                    data.registerName = registerName;
                    data.markup = registerViewMarkup(address, registerName);
                    data.title = registerViewTitle(registerName);
                    m_engine->openMemoryView(data);
              });

    addAction(this, menu, Tr::tr("Open Disassembler at 0x%1").arg(address, 0, 16),
              Tr::tr("Open Disassembler"),
              address && m_engine->hasCapability(DisassemblerCapability),
              [this, address] { m_engine->openDisassemblerView(Location(address)); });

    addAction(this, menu, Tr::tr("Open Disassembler..."),
              m_engine->hasCapability(DisassemblerCapability),
              [this, address] {
                    if (std::optional<quint64> result = runAddressDialog(address))
                        m_engine->openDisassemblerView(Location(*result));
              });

    menu->addSeparator();

    const RegisterFormat currentFormat = registerItem
            ? registerItem->m_format
            : registerSubItem
              ? registerSubItem->parent()->m_format
              : HexadecimalFormat;

    auto addFormatAction =
            [this, menu, currentFormat, registerItem](
                const QString &display, RegisterFormat format) {
            addCheckableAction(this, menu, display, registerItem, currentFormat == format,
                               [registerItem, format] {
            registerItem->m_format = format;
            registerItem->update();
        });
    };

    addFormatAction(Tr::tr("Hexadecimal"), HexadecimalFormat);
    addFormatAction(Tr::tr("Decimal"), DecimalFormat);
    addFormatAction(Tr::tr("Octal"), OctalFormat);
    addFormatAction(Tr::tr("Binary"), BinaryFormat);

    addStandardActions(ev.view(), menu);

    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(ev.globalPos());
    return true;
}

RegisterGroup *RegisterHandler::allRegisters() const
{
    RegisterGroup *bestGroup = nullptr;
    int items = 0;
    for (int i = 0, n = rootItem()->childCount(); i != n; ++i) {
        RegisterGroup *group = rootItem()->childAt(i);
        if (group->m_group == "all") // "all" group should always be
            return group;
        if (group->childCount() > items) {
            items = group->childCount();
            bestGroup = group;
        }
    }
    return bestGroup;
}

QVariant RegisterEditItem::data(int column, int role) const
{
    switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            switch (column) {
                case RegisterNameColumn: {
                    return QString("[%1]").arg(m_index);
                }
                case RegisterValueColumn: {
                    RegisterValue value = parent()->parent()->m_reg.value;
                    return value.subValue(m_subSize, m_index)
                            .toString(m_subKind, m_subSize, m_subFormat, role == Qt::EditRole);
                }
            }
            break;
        case Qt::ToolTipRole: {
                RegisterItem *registerItem = parent()->parent();
                return Tr::tr("Edit bits %1...%2 of register %3")
                        .arg(m_index * 8).arg(m_index * 8 + 7).arg(registerItem->m_reg.name);
            }
        default:
            break;
    }
    return QVariant();
}

bool RegisterEditItem::setData(int column, const QVariant &value, int role)
{
    if (column == RegisterValueColumn && role == Qt::EditRole) {
        QTC_ASSERT(parent(), return false);
        QTC_ASSERT(parent()->parent(), return false);
        RegisterItem *registerItem = parent()->parent();
        Register &reg = registerItem->m_reg;
        RegisterValue vv;
        vv.fromString(value.toString(), m_subFormat);
        reg.value.setSubValue(m_subSize, m_index, vv);
        registerItem->triggerChange();
        return true;
    }
    return false;
}

Qt::ItemFlags RegisterEditItem::flags(int column) const
{
    QTC_ASSERT(parent(), return Qt::ItemFlags());
    Qt::ItemFlags f = parent()->flags(column);
    if (column == RegisterValueColumn)
        f |= Qt::ItemIsEditable;
    return f;
}

QAbstractItemDelegate *createRegisterDelegate(int column)
{
    return new RegisterDelegate(column);
}

} // Debugger::Internal
