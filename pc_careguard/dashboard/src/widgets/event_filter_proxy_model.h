#pragma once
#include <QDate>
#include <QSortFilterProxyModel>

// "미처리만" 필터 + EMERGENCY 미처리 최상단 고정 정렬.
// CareGuard_DESIGN_SYSTEM.md §4-3: "EMERGENCY 미처리 행은 정렬과 무관하게 최상단 고정".
class EventFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit EventFilterProxyModel(QObject* parent = nullptr);

    void setUnresolvedOnly(bool unresolvedOnly);
    bool unresolvedOnly() const { return unresolvedOnly_; }

    // 로봇/대상자/담당자만 대상으로 대소문자 무시 부분일치 검색.
    void setSearchKeyword(const QString& keyword);

    // [start, end] 포함 범위. 둘 다 유효해야 필터가 걸린다.
    void setDateRange(const QDate& start, const QDate& end);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    bool unresolvedOnly_ = true;
    QString searchKeyword_;
    QDate dateStart_;
    QDate dateEnd_;
};
