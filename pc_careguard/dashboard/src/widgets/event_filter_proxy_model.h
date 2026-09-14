#pragma once
#include <QDate>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>

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

    // role=guardian 전용 — 이 이름 목록에 있는 대상자 이벤트만 보여준다. 빈 목록 = 필터 없음
    // (admin/operator). P2EventPage::setRecipientNameFilter()가 MainWindow를 통해 호출한다.
    void setRecipientNameAllowlist(const QStringList& names);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    bool unresolvedOnly_ = true;
    QString searchKeyword_;
    QDate dateStart_;
    QDate dateEnd_;
    QSet<QString> recipientAllowlist_;
};
