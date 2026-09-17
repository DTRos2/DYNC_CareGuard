#include "event_filter_proxy_model.h"

#include <QDateTime>

#include "event_table_model.h"

EventFilterProxyModel::EventFilterProxyModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
}

void EventFilterProxyModel::setUnresolvedOnly(bool unresolvedOnly) {
    if (unresolvedOnly_ == unresolvedOnly) return;
    unresolvedOnly_ = unresolvedOnly;
    invalidateFilter();
}

void EventFilterProxyModel::setSearchKeyword(const QString& keyword) {
    if (searchKeyword_ == keyword) return;
    searchKeyword_ = keyword;
    invalidateFilter();
}

void EventFilterProxyModel::setSearchEventIdOnly(bool eventIdOnly) {
    if (searchEventIdOnly_ == eventIdOnly) return;
    searchEventIdOnly_ = eventIdOnly;
    invalidateFilter();
}

void EventFilterProxyModel::setDateRange(const QDate& start, const QDate& end) {
    if (dateStart_ == start && dateEnd_ == end) return;
    dateStart_ = start;
    dateEnd_ = end;
    invalidateFilter();
}

void EventFilterProxyModel::setRecipientNameAllowlist(const QStringList& names) {
    const QSet<QString> next(names.begin(), names.end());
    if (recipientAllowlist_ == next) return;
    recipientAllowlist_ = next;
    invalidateFilter();
}

bool EventFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (!recipientAllowlist_.isEmpty()) {
        const QModelIndex idx = sourceModel()->index(sourceRow, EventTableModel::ColRecipient, sourceParent);
        if (!recipientAllowlist_.contains(sourceModel()->data(idx, Qt::DisplayRole).toString())) return false;
    }

    if (unresolvedOnly_) {
        const QModelIndex checkIdx = sourceModel()->index(sourceRow, 0, sourceParent);
        if (sourceModel()->data(checkIdx, EventTableModel::ResolvedRole).toBool()) return false;
    }

    if (dateStart_.isValid() && dateEnd_.isValid()) {
        const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        const QDate rowDate = sourceModel()->data(idx, EventTableModel::DateRole).toDate();
        if (rowDate < dateStart_ || rowDate > dateEnd_) return false;
    }

    if (!searchKeyword_.trimmed().isEmpty()) {
        const QString keyword = searchKeyword_.trimmed();
        const auto matches = [&](int column) {
            const QModelIndex idx = sourceModel()->index(sourceRow, column, sourceParent);
            return sourceModel()->data(idx, Qt::DisplayRole).toString().contains(keyword,
                                                                                  Qt::CaseInsensitive);
        };
        if (searchEventIdOnly_) {
            if (!matches(EventTableModel::ColEventId)) return false;
        } else if (!matches(EventTableModel::ColEventId) && !matches(EventTableModel::ColRobot) &&
                   !matches(EventTableModel::ColRecipient) && !matches(EventTableModel::ColAssignee)) {
            return false;
        }
    }

    return true;
}

bool EventFilterProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const int leftKind = sourceModel()->data(left, EventTableModel::RowKindRole).toInt();
    const int rightKind = sourceModel()->data(right, EventTableModel::RowKindRole).toInt();
    if (leftKind != rightKind) return leftKind < rightKind;
    // 같은 심각도 버킷 안에서는 발생일시 최신순(요청 사양) — 행 삽입 순서에 기대던
    // 예전 방식은 WS 실시간 반영·재조회 순서가 항상 시간순이라는 보장이 없었다.
    const QDateTime leftAt = sourceModel()->data(left, EventTableModel::OccurredAtRole).toDateTime();
    const QDateTime rightAt = sourceModel()->data(right, EventTableModel::OccurredAtRole).toDateTime();
    return leftAt > rightAt;
}
