#include "event_table_model.h"

#include <QFont>

#include "theme/tokens.h"

EventTableModel::EventTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int EventTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int EventTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

EventTableModel::RowKind EventTableModel::rowKind(const EventRow& row) {
    if (row.resolved) return RowKind::Normal;
    if (row.severity == Severity::Emergency) return RowKind::Emergency;
    if (row.severity == Severity::Attention) return RowKind::Attention;
    return RowKind::Normal;
}

QString EventTableModel::formatElapsed(int sec) {
    if (sec < 0) return Tokens::Format::EMPTY;
    const int h = sec / 3600;
    const int m = (sec % 3600) / 60;
    const int s = sec % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

QVariant EventTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const EventRow& row = rows_[index.row()];
    const RowKind kind = rowKind(row);

    if (role == RowKindRole) return static_cast<int>(kind);
    if (role == ResolvedRole) return row.resolved;
    if (role == IsEmergencyRole) return row.severity == Severity::Emergency;
    if (role == DateRole) return row.date;
    if (role == OccurredAtRole) return row.occurredAt;

    if (role == Qt::CheckStateRole && index.column() == ColCheck) {
        return row.checked ? Qt::Checked : Qt::Unchecked;
    }

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColCheck) return QVariant(Qt::AlignCenter);
        return QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));
    }

    if (role == Qt::ForegroundRole) {
        if (row.resolved) return Tokens::TEXT_MUTED;
        const bool severityCell = index.column() == ColSeverity || index.column() == ColType ||
                                   index.column() == ColStatus;
        if (severityCell) {
            if (row.severity == Severity::Emergency) return Tokens::EMERGENCY.main;
            if (row.severity == Severity::Attention) return Tokens::ATTENTION.main;
        }
        if (index.column() == ColElapsed && row.severity == Severity::Emergency) {
            return Tokens::EMERGENCY.main;
        }
        if (index.column() == ColAssignee && row.assignee.isEmpty()) return Tokens::TEXT_MUTED;
        return Tokens::TEXT_PRIMARY;
    }

    if (role == Qt::FontRole) {
        QFont f;
        switch (index.column()) {
            case ColEventId:
            case ColTime:
            case ColRobot:
            case ColElapsed:
                f.setFamily(Tokens::Font::MONO);
                break;
            default:
                break;
        }
        return f;
    }

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
        case ColCheck:
            return {};
        case ColSeverity:
            switch (row.severity) {
                case Severity::Emergency:
                    return QStringLiteral("긴급");
                case Severity::Attention:
                    return QStringLiteral("주의");
                default:
                    return QStringLiteral("정보");
            }
        case ColEventId:
            return row.eventId;
        case ColTime:
            return row.timeText;
        case ColRobot:
            return row.robotId;
        case ColRecipient:
            return row.recipientName;
        case ColType:
            return row.type;
        case ColSummary:
            return row.summary;
        case ColStatus:
            if (row.resolved) return QStringLiteral("완료");
            if (row.assignee.isEmpty()) return QStringLiteral("미배정");
            return QStringLiteral("대응 중");
        case ColAssignee:
            return row.assignee.isEmpty() ? Tokens::Format::EMPTY : row.assignee;
        case ColElapsed:
            return row.resolved ? Tokens::Format::EMPTY : formatElapsed(row.elapsedSec);
        default:
            return {};
    }
}

bool EventTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= rows_.size()) return false;
    if (role == Qt::CheckStateRole && index.column() == ColCheck) {
        rows_[index.row()].checked = (value.toInt() == Qt::Checked);
        // 체크 여부는 델리게이트가 행 전체(모든 컬럼)의 선택 외곽선에 반영한다. 컬럼 0
        // 셀만 dataChanged로 알리면 나머지 컬럼은 다시 그려질 계기가 없어서, 마우스가
        // 그 위를 지나가며 하나씩 갱신될 때까지 외곽선이 반쪽만 그려진 채로 남는다
        // (선택 시 초록 테두리가 끊겨 보이던 원인) — 행 전체를 무효화한다.
        emit dataChanged(this->index(index.row(), 0), this->index(index.row(), ColumnCount - 1));
        emit countsChanged();
        return true;
    }
    return false;
}

QVariant EventTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) return {};

    // 헤더 기본 정렬은 가운데인데 바디 셀은 좌측 정렬이라 컬럼이 서로 어긋나 보였다.
    // 바디와 동일한 정렬을 써서 헤더/바디 컬럼 위치를 맞춘다.
    if (role == Qt::TextAlignmentRole) {
        if (section == ColCheck) return QVariant(Qt::AlignCenter);
        return QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));
    }

    if (role != Qt::DisplayRole) return {};
    switch (section) {
        case ColCheck:
            return QString();
        case ColSeverity:
            return QStringLiteral("심각도");
        case ColEventId:
            return QStringLiteral("event_id");
        case ColTime:
            return QStringLiteral("발생일시");
        case ColRobot:
            return QStringLiteral("로봇");
        case ColRecipient:
            return QStringLiteral("대상자");
        case ColType:
            return QStringLiteral("유형");
        case ColSummary:
            return QStringLiteral("요약");
        case ColStatus:
            return QStringLiteral("상태");
        case ColAssignee:
            return QStringLiteral("담당자");
        case ColElapsed:
            return QStringLiteral("경과");
        default:
            return {};
    }
}

Qt::ItemFlags EventTableModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == ColCheck) f |= Qt::ItemIsUserCheckable;
    return f;
}

void EventTableModel::setRows(const QVector<EventRow>& rows) {
    beginResetModel();
    rows_ = rows;
    endResetModel();
    emit countsChanged();
}

void EventTableModel::tickElapsed() {
    for (int r = 0; r < rows_.size(); ++r) {
        if (rows_[r].resolved) continue;
        rows_[r].elapsedSec++;
        const QModelIndex idx = index(r, ColElapsed);
        emit dataChanged(idx, idx, {Qt::DisplayRole});
    }
}

QVector<QString> EventTableModel::checkedResolvableEventIds() const {
    QVector<QString> ids;
    for (const auto& r : rows_) {
        if (r.checked && !r.resolved && r.severity != Severity::Emergency) ids.append(r.eventId);
    }
    return ids;
}

bool EventTableModel::hasEmergencyChecked() const {
    for (const auto& r : rows_) {
        if (r.checked && r.severity == Severity::Emergency) return true;
    }
    return false;
}

void EventTableModel::resolveByEventId(const QString& eventId) {
    for (int r = 0; r < rows_.size(); ++r) {
        if (rows_[r].eventId != eventId) continue;
        rows_[r].resolved = true;
        rows_[r].checked = false;
        emit dataChanged(index(r, 0), index(r, ColumnCount - 1));
        emit countsChanged();
        return;
    }
}

void EventTableModel::clearAllChecks() {
    for (int r = 0; r < rows_.size(); ++r) rows_[r].checked = false;
    // setData()와 같은 이유로 컬럼 0뿐 아니라 행 전체를 무효화해야 선택 외곽선이
    // 한 번에 지워진다.
    if (!rows_.isEmpty()) {
        emit dataChanged(index(0, 0), index(rows_.size() - 1, ColumnCount - 1));
    }
    emit countsChanged();
}

int EventTableModel::checkedCount() const {
    int n = 0;
    for (const auto& r : rows_) n += r.checked ? 1 : 0;
    return n;
}

int EventTableModel::emergencyUnresolvedCount() const {
    int n = 0;
    for (const auto& r : rows_) n += (!r.resolved && r.severity == Severity::Emergency) ? 1 : 0;
    return n;
}

int EventTableModel::attentionUnresolvedCount() const {
    int n = 0;
    for (const auto& r : rows_) n += (!r.resolved && r.severity == Severity::Attention) ? 1 : 0;
    return n;
}

int EventTableModel::unresolvedCount() const {
    int n = 0;
    for (const auto& r : rows_) n += r.resolved ? 0 : 1;
    return n;
}
