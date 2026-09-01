#pragma once
#include <QImage>
#include <QString>

// ROS map_server 스타일 .pgm(Netpbm grayscale) 파일을 QImage로 읽는다.
// P2(ASCII) / P5(binary) 두 서브포맷을 모두 지원한다. 실패 시 null QImage 반환.
namespace PgmImageLoader {
QImage load(const QString& path);
}
