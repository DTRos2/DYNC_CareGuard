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

    // 이벤트 ID(event_uid)/로봇/대상자/담당자를 대상으로 대소문자 무시 부분일치 검색.
    // event_uid는 P2EventPage::openEventByUid(P4 "이벤트 상세로 이동")가 검색창에
    // 그대로 채워 넣는데, 예전엔 이 필터가 그 값을 매칭 대상에서 빼놓고 있어서
    // 상세 모달은 (소스 모델을 직접 훑어) 정상적으로 열려도 뒤에 남는 테이블 자체는
    // 그 이벤트가 필터에 안 걸려 안 보이는 것처럼 보였다.
    void setSearchKeyword(const QString& keyword);

    // guardian 전용(요청) — true면 검색을 event_id 컬럼 하나로만 좁힌다. 로봇·대상자·담당자는
    // guardian이 굳이 몰라도 되는 다른 사람 관련 정보라 검색 대상에서 뺀다.
    void setSearchEventIdOnly(bool eventIdOnly);

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
    bool searchEventIdOnly_ = false;
    QDate dateStart_;
    QDate dateEnd_;
    QSet<QString> recipientAllowlist_;
};
