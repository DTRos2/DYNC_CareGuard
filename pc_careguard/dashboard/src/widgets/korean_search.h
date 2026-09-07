#pragma once
#include <QString>

// 한글 초성 검색 — "ㄱㅇㅇ" 같은 초성만으로도 "김예은"이 걸리게 한다.
// "이름/코드로 검색" 형태의 필드(계정 관리 담당 대상자, 로봇 등록 담당 대상자 등)에서
// 공통으로 쓴다.
namespace KoreanSearch {

// 한글 음절의 초성만 뽑아낸 문자열로 바꾼다. 한글이 아닌 문자(코드, 로봇 번호 등)는 그대로 둔다.
inline QString choseong(const QString& text) {
    static const QString kChoseong = QStringLiteral("ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ");
    QString result;
    result.reserve(text.size());
    for (const QChar& ch : text) {
        const ushort code = ch.unicode();
        if (code >= 0xAC00 && code <= 0xD7A3) {
            const int choseongIndex = (code - 0xAC00) / (21 * 28);
            result.append(kChoseong.at(choseongIndex));
        } else {
            result.append(ch);
        }
    }
    return result;
}

// 일반 부분일치 우선, 안 걸리면 초성만으로도 대조한다.
inline bool matches(const QString& text, const QString& query) {
    if (query.isEmpty()) return true;
    if (text.contains(query, Qt::CaseInsensitive)) return true;
    return choseong(text).contains(query, Qt::CaseInsensitive);
}

}  // namespace KoreanSearch
