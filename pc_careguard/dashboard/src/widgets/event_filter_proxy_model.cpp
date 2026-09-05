#include "event_filter_proxy_model.h"

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

void EventFilterProxyModel::setDateRange(const QDate& start, const QDate& end) {
    if (dateStart_ == start && dateEnd_ == end) return;
    dateStart_ = start;
    dateEnd_ = end;
    invalidateFilter();
}

bool EventFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
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
        if (!matches(EventTableModel::ColRobot) && !matches(EventTableModel::ColRecipient) &&
            !matches(EventTableModel::ColAssignee)) {
            return false;
        }
    }

    return true;
}

bool EventFilterProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const int leftKind = sourceModel()->data(left, EventTableModel::RowKindRole).toInt();
    const int rightKind = sourceModel()->data(right, EventTableModel::RowKindRole).toInt();
    if (leftKind != rightKind) return leftKind < rightKind;
    return left.row() < right.row();
}
