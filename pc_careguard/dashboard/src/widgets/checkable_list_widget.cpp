#include "checkable_list_widget.h"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionViewItem>

CheckableListWidget::CheckableListWidget(QWidget* parent) : QListWidget(parent) {
    // 클릭은 QListWidget 자신이 아니라 viewport()로 들어온다(QAbstractItemView 구조).
    viewport()->installEventFilter(this);
}

bool CheckableListWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            QListWidgetItem* item = itemAt(mouseEvent->pos());
            if (item != nullptr && (item->flags() & Qt::ItemIsUserCheckable)) {
                QStyleOptionViewItem opt;
                opt.initFrom(this);
                opt.rect = visualItemRect(item);
                const QRect checkRect =
                    style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, this);
                // 체크박스 자체를 클릭했으면 Qt 기본 토글에 맡긴다(여기서 또 토글하면
                // 두 번 토글되어 원상태로 되돌아가 버린다). 그 외 행 어디를 클릭해도
                // 같은 효과를 내도록 여기서 대신 토글해준다.
                if (!checkRect.contains(mouseEvent->pos())) {
                    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
                }
            }
        }
    }
    return QListWidget::eventFilter(watched, event);
}
