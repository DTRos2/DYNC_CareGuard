# CareGuard 관제 대시보드 — 디자인 시스템 v1.0 (Qt 구현용)

> 이 문서는 Claude Code가 Qt 대시보드를 구현할 때 참조하는 **단일 기준(Single Source of Truth)** 이다.
> 모든 색상·크기·동작은 이 문서의 토큰을 따르며, 하드코딩된 임의 값을 새로 만들지 않는다.
> 함께 제공되는 `careguard.qss`는 이 문서의 토큰을 그대로 구현한 스타일시트다.

---

## 0. 핵심 설계 원칙 (구현 판단 기준)

1. **라이트 테마 단일.** 대시보드는 밝은 배경. 로봇 탑재 LCD 화면만 다크 (별도 §12).
2. **색은 상태 신호에만 쓴다.** 정상 상태는 무채색(흰 배경, 회색 텍스트). 화면에 색이 보이면 = 확인할 일이 있다는 뜻. 장식 목적의 색 사용 금지.
3. **상태는 3중 인코딩.** 색 + 아이콘 + 텍스트 라벨을 항상 함께 표시 (색약 대응). 색만으로 상태를 전달하는 UI 금지.
4. **위험 우선 정렬.** 목록·테이블의 기본 정렬은 위험 순. EMERGENCY 항목은 최상단 고정.
5. **파괴적 조작은 격리.** E-STOP·119 신고는 Danger 색 + 다른 버튼과 크기 차등. 일반 버튼과 나란히 같은 크기로 두지 않는다.
6. **숫자는 tabular(고정폭 숫자).** 테이블·카운터의 숫자 폰트는 자릿수 정렬 유지.

---

## 1. 컬러 토큰

### 1-1. 베이스 (라이트)

| 토큰 | HEX | 용도 |
|---|---|---|
| `BG_BASE` | `#F5F7F9` | 앱 최하단 배경 (QMainWindow central) |
| `BG_SURFACE` | `#FFFFFF` | 카드·테이블·패널 배경 |
| `BG_SUNKEN` | `#EDF1F4` | 테이블 헤더·입력 필드 배경 |
| `BG_HOVER` | `#F0F4F7` | 행/카드 hover 배경 |
| `BORDER` | `#E3E8ED` | 기본 구분선 |
| `BORDER_STRONG` | `#CDD5DD` | 테이블 헤더 하단·강조 경계 |

### 1-2. 텍스트

| 토큰 | HEX | 용도 |
|---|---|---|
| `TEXT_PRIMARY` | `#16202B` | 본문·수치 |
| `TEXT_SECONDARY` | `#5A6774` | 라벨·보조 설명 |
| `TEXT_MUTED` | `#93A0AC` | 비활성·플레이스홀더 |
| `TEXT_INVERSE` | `#FFFFFF` | 채움 버튼 위 텍스트 |

### 1-3. 브랜드 액센트 (청록)

| 토큰 | HEX | 용도 |
|---|---|---|
| `ACCENT` | `#0E9384` | Primary 버튼, 활성 탭/메뉴, 선택 표시 |
| `ACCENT_HOVER` | `#0B7A6E` | Primary 버튼 hover |
| `ACCENT_SOFT` | `#E0F5F2` | 활성 메뉴 배경, 선택 배경 |
| `ACCENT_SOFT_BORDER` | `#B9E2DB` | ACCENT_SOFT 위 테두리 |
| `ACCENT_BRIGHT` | `#2DD4BF` | **로봇 LCD 화면 전용** (대시보드 사용 금지) |

### 1-4. 상태 색 — 시스템의 핵심

| 상태 | 텍스트/아이콘 | 배경(soft) | 테두리 | 트리거 |
|---|---|---|---|---|
| `EMERGENCY` | `#D93B47` | `#FDECEE` | `#F5C2C7` | 심박 임계 이탈, 음성 무응답, 도움 요청 |
| `ATTENTION` | `#C27500` | `#FFF6E5` | `#F2DCAE` | 복약 미수령, 웨어러블 미착용/저전력, 만료 임박 |
| `NORMAL_OK` | `#16875A` | `#E8F6EF` | `#BCE3D0` | **완료 표시 전용** (수령 ✓, RESOLVED, 온라인 점) |
| `INFO` | `#2563A8` | `#E9F2FB` | `#C2D9F0` | 진행 중, 다음 예정, 경로 편집 모드 |
| `OFFLINE` | `#8A97A3` | `#F1F4F6` | `#DFE5EA` | 통신 두절, 비활성 |

**규칙:**
- 정상 상태의 행/카드는 색을 칠하지 않는다 (흰 배경 유지). `NORMAL_OK` 초록은 완료 마크·온라인 점에만.
- EMERGENCY 펄스: 배경을 `#FDECEE ↔ #FBDDE0` 로 2초 주기 교차 (QPropertyAnimation 또는 QTimer, §9 참조).
- 상태 우선순위 (한 항목에 복수 상태 시): `EMERGENCY > ATTENTION > OFFLINE > NORMAL`.

### 1-5. 그림자

| 토큰 | 값 (QGraphicsDropShadowEffect 기준) | 용도 |
|---|---|---|
| `SHADOW_SM` | blur 8, dy 1, `rgba(22,32,43,0.06)` | 카드 |
| `SHADOW_MD` | blur 24, dy 4, `rgba(22,32,43,0.10)` | 드롭다운, 위험 카드 |
| `SHADOW_LG` | blur 48, dy 12, `rgba(22,32,43,0.16)` | 모달, 오버레이 패널 |
| 모달 백드롭 | `rgba(22,32,43,0.45)` | 반투명 오버레이 위젯 |

---

## 2. 타이포그래피

**폰트 로드 순서 (QFontDatabase로 번들 ttf 등록 후 사용):**
- 본문: `Pretendard` → 없으면 `Noto Sans KR` → `맑은 고딕`
- 모노: `JetBrains Mono` → `D2Coding` → `Consolas`

**모노 폰트 필수 대상:** 로봇 ID(R-01), event_id, 좌표, 시각(HH:MM:SS), IP, 네임스페이스, bpm 수치 옆 시각 등 육안 대조가 필요한 모든 식별자.

| 스타일 | 크기(pt 기준 px) | 굵기 | 용도 |
|---|---|---|---|
| `DISPLAY` | 32px | 700 | 심박수 대형 숫자 (대상자 카드) |
| `H1` | 24px | 600 | 페이지 타이틀 |
| `H2` | 18px | 600 | 섹션·모달 타이틀 |
| `H3` | 15px | 600 | 카드 타이틀·탭 |
| `BODY` | 14px | 400 | 본문·테이블 셀 |
| `BODY_S` | 13px | 400 | 보조 설명 |
| `LABEL` | 12px | 500 | 컬럼 헤더·폼 라벨 (자간 +0.3px) |
| `CAPTION` | 11px | 400 | 타임스탬프·단위 |
| `MONO` | 13px | 400 | 식별자류 |

숫자 표시 위젯은 `QFont.setStyleHint` 후 `font.setFeatures` 불가 시, 모노 폰트로 대체하여 tabular 효과를 확보한다.

---

## 3. 스페이싱 · 레이아웃 상수

4px 기반 스케일: `4 · 8 · 12 · 16 · 24 · 32 · 48 · 64`

| 상수 | 값 |
|---|---|
| `SIDEBAR_W` | 240 (접힘 64) |
| `TOPBAR_H` | 48 |
| `PAGE_PADDING` | 24 |
| `CARD_PADDING` | 16 |
| `CARD_GAP` | 16 |
| `TABLE_ROW_H` | 48 (컴팩트 40) |
| `MODAL_W_S / M / L` | 480 / 720 / 1040 |
| `RADIUS_SM / MD / LG` | 4 / 8 / 12 |
| 최소 해상도 | 1440 × 900 (QMainWindow.setMinimumSize) |

**공통 셸 구조 (모든 페이지 동일):**
```
QMainWindow
└─ centralWidget (BG_BASE)
   ├─ TopBar   (objectName="topBar", 높이 48, BG_SURFACE, 하단 BORDER)
   └─ QHBoxLayout
      ├─ Sidebar (objectName="sidebar", 폭 240, BG_SURFACE, 우측 BORDER)
      └─ PageStack (QStackedWidget, 패딩 24)
```

**TopBar 구성 (좌→우):** 로고 "CareGuard"(ACCENT, H3) · 미처리 긴급 뱃지(EMERGENCY 채움, 흰 텍스트) · 주의 뱃지(ATTENTION soft) · [stretch] · 현재시각(MONO) · 담당자명. **배경은 항상 흰색 고정** — 긴급 시에도 배경색을 바꾸지 않는다 (뱃지가 신호 담당).

**Sidebar 메뉴 (QPushButton checkable, autoExclusive):**
```
── 관제 ──   통합 관제 / 이벤트 / 대상자
── 기록 ──   영상
── 설정 ──   맵·로봇 / 순찰·경로 / 시스템
[stretch]
─────────   로그아웃 / 종료(빨간 텍스트, QApplication.quit 전 확인 다이얼로그)
```
활성 메뉴: ACCENT_SOFT 배경 + ACCENT 텍스트 + 굵기 500. 그룹 라벨은 CAPTION, TEXT_MUTED.

---

## 4. Qt 구현 규약 (필수)

### 4-1. 스타일 적용 방식
- 전역 QSS 1개 파일(`careguard.qss`)을 `app.setStyleSheet()`로 로드. 위젯별 inline setStyleSheet 남발 금지.
- 위젯 변형은 **objectName + 동적 프로퍼티** 조합으로 선택한다.
  - 예: `btn.setProperty("variant", "primary")` → QSS `QPushButton[variant="primary"]`
  - 예: `card.setProperty("status", "emergency")` → QSS `QFrame#recipientCard[status="emergency"]`
- **동적 프로퍼티 변경 후 반드시 재폴리시:**
  ```cpp
  widget->setProperty("status", "emergency");
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  ```
  프로퍼티 값은 `const char*` 리터럴 대신 `theme/tokens.h`의 상수 사용을 권장 (오타 방지).

### 4-2. 표준 objectName / 프로퍼티 목록

| 위젯 | objectName | 프로퍼티 | 값 |
|---|---|---|---|
| 상단 바 | `topBar` | — | — |
| 사이드바 | `sidebar` | — | — |
| 사이드바 메뉴 | `navBtn` | `checked` | true/false |
| 페이지 타이틀 | `pageTitle` | — | — |
| KPI 카드 | `kpiCard` | `level` | none/emergency/attention |
| 상태 배지 | `badge` | `level` | emergency/attention/ok/info/offline |
| 버튼 | — | `variant` | primary/secondary/danger/ghost |
| 카드 | `card` | — | 일반 흰 카드 |
| 대상자 카드 | `recipientCard` | `status` | normal/attention/emergency/offline |
| 입력 | `input` | `state` | normal/focus/error |
| 모달 컨테이너 | `modalFrame` | — | — |
| 인라인 액션 링크 | `linkBtn` | — | ACCENT 텍스트 |

### 4-3. 테이블 (QTableView + 커스텀 델리게이트)
QSS만으로는 데이터 기반 행 하이라이트가 불가하므로 **모델의 `Qt.BackgroundRole` 또는 QStyledItemDelegate로 구현**한다.

행 상태별 스타일 (P1 통합 상태·P2 이벤트 큐 공통):

| 상태 | 좌측 바 | 행 배경 | 텍스트 |
|---|---|---|---|
| EMERGENCY | 4px `#D93B47` | `#FDECEE` (+펄스) | TEXT_PRIMARY, 상태 셀만 EMERGENCY |
| ATTENTION | 4px `#C27500` | `#FFF6E5` | TEXT_PRIMARY |
| NORMAL | 없음 | `#FFFFFF` | TEXT_PRIMARY |
| OFFLINE | 없음 | `#F1F4F6` | TEXT_MUTED 전체 |
| 완료(이벤트) | 없음 | `#FFFFFF` | TEXT_MUTED 전체 |

- 좌측 바는 델리게이트 paint에서 `rect.left()` 기준 4px 사각형으로 그린다.
- 헤더: BG_SUNKEN 배경, LABEL 스타일, 하단 1px BORDER_STRONG.
- 행 높이 48, hover 시 BG_HOVER (단, 상태 배경이 있는 행은 상태색 유지).
- EMERGENCY 미처리 행은 정렬과 무관하게 최상단 고정 (모델 정렬 로직에서 처리, "sticky"는 프록시모델 커스텀 정렬로 구현).
- 선택 행: 배경 유지 + 1.5px ACCENT 외곽선 (델리게이트).
- P2 체크박스 컬럼: 0번 컬럼 `Qt.CheckStateRole`. 체크박스 셀 클릭=선택, 나머지 셀 클릭=상세 모달 오픈 (viewport 이벤트에서 컬럼 분기).
- **일괄 완료 처리에서 EMERGENCY 건은 제외** — 체크는 가능하되 실행 시 목록에서 걸러내고 안내 표시.

### 4-4. 모달 (QDialog)
- 프레임리스 아님. 표준 QDialog + `setModal(True)`. 배경 흐림은 부모 위 반투명 오버레이 QWidget(`rgba(22,32,43,115)`)을 띄우고 그 위에 다이얼로그.
- 구조: 헤더(높이 46~56, 하단 BORDER) → 본문(내부 스크롤, 최대 높이 70% 화면) → 푸터(높이 52~64, 상단 BORDER, BG_BASE, 우측 정렬 버튼).
- ESC 닫기 허용. 단 **완료 처리·저장 등 입력 진행 중에는 백드롭 클릭 닫기 비활성** (오버레이 클릭 무시 플래그).
- 탭 있는 모달(P3): 헤더 아래 QTabBar 40px, 활성 탭 = ACCENT 하단 2px 보더 + ACCENT 텍스트.
- 모달 폭: 이벤트/영상/대상자 상세 = M(620~720), 파일 등록 = S~M(560), 확인 다이얼로그 = S(480).

### 4-5. 버튼

| variant | 배경 | 텍스트 | 테두리 | 높이 |
|---|---|---|---|---|
| primary | ACCENT (hover: ACCENT_HOVER) | 흰색 | 없음 | 36 |
| secondary | 흰색 (hover: BG_HOVER) | TEXT_PRIMARY | BORDER_STRONG | 36 |
| danger | `#D93B47` (hover: `#C22F3A`) | 흰색 | 없음 | 36 |
| ghost | 투명 (hover: BG_HOVER) | TEXT_SECONDARY | 없음 | 36 |

- 크기 변형: S=28, 기본=36, L=44. **E-STOP·전체 비상정지·119 모의 신고 = L 사이즈 danger 고정.**
- 비활성: BG_SUNKEN 배경 + TEXT_MUTED (예: "RESOLVED 전환 (메모 입력 후 활성화)", "등록·배포 (.yaml 업로드 후 활성화)").
- 반경 RADIUS_MD(8). 배지·소형 칩은 RADIUS_SM(4).

### 4-6. 입력 (QLineEdit/QComboBox/QTextEdit)
- 배경 흰색, 테두리 1px BORDER_STRONG, 반경 6~8, 높이 28~36, 패딩 좌우 8.
- 포커스: 테두리 ACCENT + (가능 시) 외곽 글로우 `rgba(14,147,132,0.35)`.
- 오류: 테두리 `#D93B47` + 하단 CAPTION 오류 문구.
- 숫자 입력(임계값·속도): QDoubleSpinBox/QSpinBox, 모노 폰트, 우측 정렬, 단위 라벨은 입력 밖 우측.

### 4-7. 토글 스위치
QCheckBox를 커스텀 paint하거나 QSS indicator로 구현: 트랙 26×14 RADIUS_FULL, ON = ACCENT 채움 + 흰 노브 우측, OFF = BORDER_STRONG 채움 + 노브 좌측.

### 4-8. 요일 칩 (스케줄)
QPushButton checkable 7개, 22×22, RADIUS 5. checked = ACCENT 채움 + 흰 텍스트, unchecked = 흰 배경 + BORDER + TEXT_MUTED.

---

## 5. 핵심 컴포넌트 스펙

### 5-1. 상태 배지 (`badge`)
```
[아이콘 12px][텍스트 12px/500]  높이 24 · RADIUS 4 · 좌우 패딩 8
level별: 배경=soft색 · 텍스트=본색 · 테두리=테두리색
EMERGENCY 강조형(상단바·썸네일): 본색 채움 + 흰 텍스트
아이콘 매핑: emergency=8각 경고 / attention=삼각 경고 / ok=체크 / info=재생·점 / offline=플러그 해제
텍스트: 긴급 / 주의 / 완료(또는 정상) / 진행 중 / 오프라인
```

### 5-2. 대상자 카드 (`recipientCard`) — P3
```
최소 폭 320 · RADIUS 12 · 패딩 16 · 그리드 auto-fill
status=normal    : 흰 배경 + 1px BORDER + SHADOW_SM
status=attention : #FFF6E5 배경 + 1.5px #C27500 테두리        ← 위험 카드와 동일 구조, 색만 다름
status=emergency : #FDECEE 배경 + 1.5px #D93B47 테두리 + 펄스 + SHADOW_MD
status=offline   : #F1F4F6 배경 + opacity 0.6 효과(QGraphicsOpacityEffect 0.75) + 회색 텍스트
```
내부 구성(위→아래): 이름 + 상태 배지 / ID·로봇·가정(MONO, CAPTION) / **심박 DISPLAY 숫자 + 스파크라인(120×32)** / 상태 요약 한 줄 / 복약 3칸 인디케이터 + 웨어러블 상태 / 퀵액션 버튼(상태별 §7).

### 5-3. 복약 3칸 인디케이터
```
칸 3개(아침·점심·저녁) 각 22~28px 정사각, RADIUS 4, 간격 4
수령완료: NORMAL_OK soft 배경 + ✓  /  진행중: INFO 테두리 + ⏳
예정: BORDER 테두리 빈칸        /  미수령: #D93B47 채움 + 흰 ✕
```

### 5-4. 스파크라인·심박 그래프 (Qt C++)
- 스파크라인(카드, 120×32): **QPainter 직접 구현** 권장 — 의존성 없이 가볍고 카드당 인스턴스가 많아 QCustomPlot은 과함. `paintEvent`에서 QPolygonF 폴리라인 + 임계 점선.
- 상세 그래프(모달): **QCustomPlot** (단일 헤더/소스, MIT 아님 GPL/상용 주의 — 포폴 용도 GPL 가능) 또는 Qt Charts 모듈. 이탈 밴드는 QCPItemRect / QAreaSeries로.
- 스파크라인(카드): 폭 120 × 높이 32, 선 2px, 최근 30분. 정상 구간 `#16875A`, 임계 이탈 구간 `#D93B47` 세그먼트 분리. 임계 하한 점선 가이드 `#93A0AC`.
- 상세 그래프(모달): 임계 상·하한 `#C27500` 점선, 정상 밴드 `#E8F6EF` 반투명 채움, 이탈 구간 `#FDECEE` 영역 + 이탈 시작 세로 점선 `#D93B47`.
- 신호 없음: `#93A0AC` 점선 수평선.

### 5-5. 센서 인디케이터
8px 원형 점 + CAPTION 라벨. **정상 = `#CDD5DD` 회색** (초록 아님) / 이상 = `#D93B47` / 미확인 = OFFLINE. 이상만 눈에 띄게.

### 5-6. 프로필 사진 (대상자)
**노출 위치: P3 대상자 상세 모달 탭 ① 기본 정보 단 한 곳.** 카드·테이블·이벤트 상세 등 다른 화면에는 표시하지 않는다 (확정 사항 — 임의 확장 금지).
```
크기 96×96 · RADIUS 12 · 배경 BG_SUNKEN · 테두리 BORDER_STRONG
미등록: 회색 인물 실루엣 플레이스홀더 + [사진 등록] 버튼
등록됨: 이미지 + 하단 반투명 라벨 "사진 등록됨" + [변경]/[삭제(빨간 텍스트)] 버튼
안내문: "JPG·PNG / 최대 5MB" (CAPTION, TEXT_MUTED)
```
- 업로드 시 **중앙 기준 정사각 크롭 후 저장. 원본은 보관하지 않는다** (개인정보 최소 보관 — P4 영상 정책과 동일 원칙).
- 저장: 파일은 서버(또는 로컬 데이터 디렉토리), DB에는 경로만 (`recipient.photo_path`). 삭제 시 파일도 즉시 제거.
- Qt 구현: QLabel에 QPixmap, `QPainterPath::addRoundedRect` 클리핑으로 라운드 처리. 로드 실패 시 플레이스홀더로 폴백.

### 5-7. 브랜드 심볼 (로고)

**자산 2종을 용도별로 분리 운용한다.**

| 용도 | 자산 | 비고 |
|---|---|---|
| 문서 표지·발표 자료 | 골드 엠블럼 (다크 배경 전용, 래스터) | 기존 표지 이미지. **UI에 사용 금지** |
| 대시보드 UI · LCD | **청록 단색 SVG 심볼** `careguard_mark.svg` | 방패 + 로봇(안테나·눈·미소) 외곽선 |

```
형태  : 방패 외곽선 안에 로봇 얼굴. 스트로크 전용(면 채움 없음), 라운드 조인
색    : 대시보드 ACCENT #0E9384 / LCD LCD_ACCENT #2DD4BF (형태 동일, 색만 교체)
스트로크: 46px 기준 2.0px → 20px 이하에서는 3.0px로 굵혀 가독성 확보
크기  : 로그인 46 (76px ACCENT_SOFT 라운드 배경 위) / 상단 바 18 / LCD 16
워드마크: "CareGuard" — 심볼 우측 gap 10, weight 600, 자간 -0.2px, ACCENT 색
```
- 구현: QSvgRenderer로 로드 후 런타임 착색. 색상별 SVG 사본을 만들지 않는다.
- 상단 바에서는 심볼+워드마크 락업, 사이드바 접힘 상태에서는 심볼 단독.

### 5-8. 로그인 화면 (P0)

**진입:** 앱 최초 실행 시 항상 표시. 인증 성공 전에는 다른 화면에 접근할 수 없다.

**레이아웃**
```
전체 BG_BASE · 최상단 3px ACCENT 라인
중앙 카드: 폭 340 · RADIUS 12 · 패딩 26/28 · BG_SURFACE · SHADOW_LG
구성(위→아래): 심볼(76px 배경 위 46px) → "CareGuard" 21px/600 ACCENT
              → 서브 "노인 안전 돌봄 로봇 · 통합 관제" CAPTION TEXT_MUTED
              → 아이디(MONO, user 아이콘) → 비밀번호(lock 아이콘 + eye 토글)
              → [로그인 유지] 체크박스 → 로그인 버튼(전폭 40px primary)
```
- **카드 하단에 어떤 문구도 두지 않는다** (버전·제작사·문의처·회원가입·비밀번호 찾기 전부 없음). 계정은 P7에서 관리자가 발급한다.
- Enter 키 = 로그인 실행. 최초 포커스는 아이디 필드.

**상태 4종**

| 상태 | 표현 | 버튼 |
|---|---|---|
| ① 인증 실패 | 비밀번호 필드 1.5px EMERGENCY 테두리 + 하단 오류 문구 `아이디 또는 비밀번호가 올바르지 않습니다 (n/5)` | 활성 유지 |
| ② 계정 잠금 (5회) | 입력 영역을 ATTENTION 배너로 교체: "계정이 잠겼습니다 / {MM:SS} 후 재시도하거나 관리자에게 문의하세요" | 비활성 |
| ③ 서버 연결 실패 | EMERGENCY 배너: "관제 서버에 연결할 수 없습니다 / 네트워크 상태를 확인해 주세요" | `다시 시도`(secondary)로 교체 |
| ④ 인증 중 | 입력 필드 비활성(BG_BASE) | ACCENT_HOVER + 스피너 + "확인 중…" |

**정책**
- 실패 문구는 아이디/비밀번호 중 **어느 쪽이 틀렸는지 알리지 않는다.**
- 5회 연속 실패 시 5분 잠금. 카운트다운은 MONO 표기, 만료 시 자동으로 기본 상태 복귀.
- **③ 서버 오류는 실패 카운트에 포함하지 않는다** — 서버 장애로 계정이 잠기는 사고 방지.
- ④ 상태에서 입력·버튼을 모두 잠가 중복 제출을 막는다.
- 로그인 성공/실패/잠금 모두 **감사 로그 기록**(P7 탭④).
- `로그인 유지` 체크 시 세션 토큰 로컬 저장, 미체크 시 앱 종료로 세션 만료.
- 성공 → P1 통합 관제 진입, TopBar에 담당자명 표시.

### 5-9. KPI 카드 (P1 상단)
```
높이 ~64 · RADIUS 8 · 패딩 8~12 · 라벨 CAPTION + 값 H2급
값 0 또는 정상 → level=none: 흰 배경, 무채색
값 >0 위험/주의 → level에 따라 soft 배경 + 본색 텍스트
클릭 시 하단 테이블에 해당 필터 적용
```

---

## 6. 페이지 인벤토리 (QStackedWidget 인덱스)

| idx | 페이지 | 핵심 위젯 | 모달 |
|---|---|---|---|
| — | **P0 로그인** | 앱 최초 화면. 인증 성공 전 다른 화면 접근 불가 (§5-8) | — |
| 0 | P1 통합 관제 | KPI 스트립(5) → 통합 상태 QTableView → 알림 피드 리스트. 우상단 긴급 조작 바 | 로봇 상세(미니맵+노드+제어) |
| 1 | P2 이벤트 관제 | 필터 바 → (선택 시) 일괄 처리 바 → 체크박스 이벤트 QTableView → **가운데 정렬 페이지네이션** | 이벤트 상세(진행바+심박+음성+조치+메모) / 통계 |
| 2 | P3 대상자 관리 | 검색·정렬 토글(위험/이름/로봇별) → 카드 그리드(FlowLayout) | 대상자 상세 3탭(기본/웨어러블·임계값/복약) |
| 3 | P4 영상 관리 | 필터 + 용량 게이지 → 썸네일 그리드↔테이블 토글 → 정책 배너 | 재생 모달(**사유 입력 전 잠금 오버레이**) |
| 4 | P5 맵·로봇 매핑 | 로봇 셀렉터 + 파일등록 버튼 → **매핑 현황 오버레이 드롭다운(레이아웃 밀지 않음)** → SLAM 캔버스(대형) + 텔레옵 패널 | 파일 등록(.pgm/.yaml 쌍 검증) |
| 5 | P6 순찰·경로 | 맵 셀렉터 → WP 편집 캔버스 + 우측(WP 목록/경로/스케줄) | WP 팝오버, 경로 편집 모드(파랑), 스케줄 인라인 폼 |
| 6 | P7 시스템 설정 | QTabWidget 4탭: 로봇 관리 / 계정 관리 / 임계값·알림 / 감사 로그 | 로봇 등록, 계정 등록 |

**페이지별 구현 주의점:**
- P1: 알림 피드 행 클릭 → P2로 전환 + 해당 이벤트 모달 오픈. 테이블 행 클릭 → 로봇 상세 모달. 지도는 P1에 없음 (로봇 상세 모달 안 미니맵만).
- **P1 행별 비상정지** (테이블 최우측 `정지` 전용 컬럼, 액션 컬럼과 분리):
  - 스타일은 **테두리형**(흰 배경 + 1px EMERGENCY 테두리·텍스트, 높이 26). 채움형은 상단 `전체 비상정지`(L 사이즈 danger 채움) 전용 — 행마다 채움 버튼을 두면 EMERGENCY 행 배경 신호가 묻힌다.
  - 클릭 시 **확인 다이얼로그 필수**: "R-01 (김순자 / 행복빌라 302)을 비상정지합니다. 진행 중인 미션이 중단됩니다." → [취소] / [정지(danger)]
  - 실행 후: 행 상태를 `E-STOP` 배지(EMERGENCY 채움)로 전환, 버튼은 `해제`(secondary)로 교체. 해제도 확인 다이얼로그를 거친다.
  - **OFFLINE 행은 버튼 비활성** + 툴팁 "통신 두절 — 정지 명령 전달 불가".
  - 실행·해제 모두 감사 로그 기록(§7 / P7 탭④).
- P2: 기본 필터 "미처리만" ON. 경과 시간은 미처리 건만 1초 QTimer 카운트업, EMERGENCY는 빨간 숫자.
- P3: 정렬 "위험 순" 기본. `로봇별` 선택 시 로봇 ID 그룹 헤더로 묶음. 프로필 사진은 상세 모달 탭 ①에서만 표시·관리 (§5-6).
- P4: 잠금 오버레이는 사유 텍스트 입력 후 재생 버튼 활성 → 열람 이력에 즉시 기록. 열람 제한 카운트다운 표시.
- P5: 텔레옵 = 방향키(↑↓←→) + Space 정지, 키 눌림 시 해당 키패드 버튼 ACCENT_SOFT 하이라이트. 속도 = `라벨 | QDoubleSpinBox | 슬라이더` 한 줄 (입력↔슬라이더 양방향 동기화) + 프리셋 버튼 3개(미세 0.10/0.3, 보통 0.30/0.5, 빠름 0.50/0.8 — 선속 m/s / 각속 rad/s, 실측 후 조정). 매핑 현황 펼침은 자식 오버레이 위젯(raise_)로 캔버스 위에 띄운다.
- P6: WP 유형 = 순찰 지점 / 충전 스테이션 2종만. 충전 스테이션 = 초기 위치, 맵당 1개, 경로 순서에 포함 불가(자동 출발·복귀). 경로 편집 모드는 INFO 파랑으로 모드 전환을 표시.
- P7: 보호자 계정은 스키마만 — 비활성 고정. 감사 로그에서 영상 열람·이벤트 완료·E-STOP·119는 EMERGENCY 색 강조.

---

## 7. 상태 판정 · 액션 매핑 로직

### 7-1. 대상자/행 상태 판정 (우선순위 순)
```
if 심박 임계 이탈(지속시간 초과) or 음성 무응답/도움요청 → EMERGENCY
elif 복약 미수령 or 웨어러블 미착용 or 웨어러블 배터리<20% → ATTENTION
elif 로봇·웨어러블 통신 두절                            → OFFLINE
else                                                    → NORMAL
```

### 7-2. 상태별 퀵액션 (카드·테이블 공통 — "지금 할 일이 첫 버튼")
| 상태 | 첫 버튼 | 보조 |
|---|---|---|
| EMERGENCY | 이벤트 확인 (danger) | 보호자 연락 |
| ATTENTION(복약) | 복약 재안내 (primary) | 보호자 연락 |
| OFFLINE | 재연결 시도 (primary) | 보호자 연락 |
| NORMAL | 상세 보기 (secondary) | — |

---

## 8. 아이콘
- 세트: **Lucide** (또는 Tabler) 단일 세트, 스트로크 1.5px, 기본 16px / 툴바 20px. SVG를 QIcon으로 로드, 색은 상태색에 맞춰 런타임 착색(QPainter composition 또는 색상별 SVG 사본).
- 주요 매핑: 로봇=bot / 대상자=user-heart / 심박=activity / 복약=pill / 맵=map / 경로=route / 영상=video / 경보=bell / 긴급=octagon-alert / 오프라인=plug-off / 설정=settings.

---

## 9. 모션

| 상황 | 값 |
|---|---|
| 기본 트랜지션(hover 등) | QSS 자체 전환 없음 — 즉시 변경 허용 |
| 모달 등장 | 200ms fade (QPropertyAnimation windowOpacity 0→1) |
| EMERGENCY 펄스 | 2s 주기 배경 교차 (QVariantAnimation color loop) |
| 데이터 갱신 플래시 | 300ms 배경 하이라이트 후 복귀 |

- 페이지 전환 슬라이드·장식 애니메이션 금지.
- 설정에 "모션 줄이기" 옵션 제공 시 펄스를 정적 강조(테두리 2px)로 대체.

---

## 10. 데이터 표기 규칙

| 항목 | 형식 |
|---|---|
| 시각 | `HH:MM` (초 필요 시 `HH:MM:SS`), MONO |
| 날짜+시각 | `MM.DD HH:MM` |
| event_id | `EV-YYMMDD-NNN`, MONO |
| 로봇/대상자 ID | `R-NN` / `C-NNN`, MONO |
| 심박 | 정수 + ` bpm` (단위는 작게 CAPTION) |
| 경과 시간 | `MM:SS` 또는 `H:MM:SS`, 미처리만 카운트업 |
| 빈 값 | `—` (TEXT_MUTED) |
| 만료 | `만료 D-N`, D-7 이하 ATTENTION 색 |

---

## 11. 금지 사항 (Do NOT)

1. 다크 배경을 대시보드에 사용하지 않는다 (LCD 화면 제외).
2. `ACCENT_BRIGHT(#2DD4BF)`를 대시보드에 사용하지 않는다.
3. 정상 상태에 초록 배경을 칠하지 않는다.
4. 상태를 색만으로 표시하지 않는다 (아이콘+텍스트 필수).
5. E-STOP/119 버튼을 일반 버튼과 동일 크기·색으로 두지 않는다.
6. EMERGENCY 이벤트를 일괄 완료 처리에 포함하지 않는다.
7. 헤더(TopBar) 배경색을 상태에 따라 바꾸지 않는다.
8. 영상 재생을 사유 입력 없이 허용하지 않는다.
9. 충전 스테이션 웨이포인트를 경로 순서에 넣지 않는다.
10. 토큰에 없는 색·크기를 임의로 추가하지 않는다. 필요 시 이 문서에 먼저 추가한다.
11. 대상자 프로필 사진을 탭 ① 기본 정보 외의 화면(카드·테이블·이벤트·영상 등)에 표시하지 않는다.
12. 문서 표지용 골드 엠블럼을 대시보드·LCD UI에 사용하지 않는다 (UI는 청록 단색 SVG 심볼만 — §5-7).
13. 로그인 카드 하단에 버전·제작사·문의처·회원가입·비밀번호 찾기 등 어떤 문구도 추가하지 않는다.
14. 서버 연결 실패를 로그인 실패 횟수에 포함하지 않는다.

---

## 12. 로봇 탑재 LCD 화면 (별도 테마 — 다크)

대시보드와 분리된 화면 세트. LIMO LCD 해상도에 맞춰 풀스크린 Qt 앱.

| 토큰 | HEX |
|---|---|
| `LCD_BG` | `#0F1419` |
| `LCD_SURFACE` | `#161C24` |
| `LCD_TEXT` | `#E8EDF2` |
| `LCD_TEXT_SUB` | `#9AA7B4` |
| `LCD_ACCENT` | `#2DD4BF` |
| `LCD_DANGER` | `#F4525C` |
| `LCD_WARN` | `#F0A73A` |

### 12-1. 공통 원칙 (대시보드와 다른 점)

1. **사용자 = 고령자.** 화면당 전달 정보 1가지. 정보 밀도를 대시보드처럼 높이지 않는다.
2. **문구는 존댓말 구어체.** 시스템 상태가 아닌 **사용자 행동 안내**로 번역해 표기한다.
   - 나쁜 예: "PATROLLING", "배터리 임계 미달로 순찰 중단"
   - 좋은 예: "거실로 이동하고 있어요", "배터리 부족으로 순찰을 쉬고 있어요 · 충전 중"
3. **글자 크기 최소값 (실기기 px):** 본문 20px / 헤드라인 32px+ / 터치 버튼 텍스트 28px+ / 전화번호 28px+ (119는 40px+). 보조 설명(CAPTION급)만 16px 허용.
4. **터치 타깃 최소 120×120px.** 터치 요소 간 간격 최소 16px.
5. **색-의미 고정:** 긍정/정상 = `LCD_ACCENT` 청록, 도움/긴급 = `LCD_DANGER` 빨강, 이상·안내 = `LCD_WARN` 앰버. 화면마다 색 의미를 바꾸지 않는다.
6. 전체 풀스크린 Qt 앱 (커서 숨김, 화면 꺼짐 방지). 구현은 대시보드와 동일하게 QSS + tokens.h `Tokens::Lcd` 사용.

### 12-2. 화면 상태 머신

```
                 ┌──────────────┐
   평시/복귀 ───▶│ ① 대기/순찰   │◀─── 각 화면 종료 시 복귀 (기본 화면)
                 └──┬────┬────┬─┘
   순찰불가·오류 ──▶ ② 연락망      (조건 해소 시 ①로)
   복약 시각 도달 ─▶ ③ 복약 안내   (수령확인 or 미수령확정 → ①)
   심박 이상 출동 ─▶ ④ 음성 확인   (응답 or 무응답확정 → ① 또는 ②)
```
우선순위: **④ 음성 확인 > ② 연락망 > ③ 복약 > ①**. 상위 화면 조건 발생 시 하위 화면을 즉시 대체한다 (예: 복약 안내 중 심박 이상 → ④로 전환).

### 12-3. 화면 ① 대기 / 순찰 (기본 화면)

| 구성 | 스펙 |
|---|---|
| 상단 바 | "CareGuard"(LCD_ACCENT) · 로봇 ID(MONO) · 현재 시각(MONO) |
| 중앙 | 상태 원형 아이콘(회전 링 애니메이션, 순찰 중일 때만) + 상태명 32px + 안내 한 줄 20px |
| 하단 | 배터리 게이지(LCD_ACCENT, 20%↓ LCD_WARN) + "다음 일정 · HH:MM {일정명}" |

**상태명 문구 매핑:** IDLE→"대기 중" / PATROLLING→"순찰 중"+"{장소}로 이동하고 있어요" / 충전 중→"충전 중" / MED_MISSION 이동→"약을 가져가고 있어요".
**전환:** 다른 화면 조건 발생 시 즉시 이탈. 다음 일정 표시로 로봇 접근을 예고해 놀람을 방지한다.

### 12-4. 화면 ② 연락망 표출

**진입 조건:** 순찰 불가(배터리<20%, Nav2 실패 반복, 센서 오류) 또는 관제 통신 두절. **LED 경광 점멸과 동기화.**

| 구성 | 스펙 |
|---|---|
| 프레임 | LCD_WARN 2px 테두리 (긴급 상황 아님 — 앰버) |
| 헤드라인 | "지금은 도움이 필요하면 전화해 주세요" (LCD_WARN, 28px+) — "고장"이라 쓰지 않는다 |
| 사유 한 줄 | 원인을 구어체로: "배터리 부족으로 순찰을 쉬고 있어요 · 충전 중" (16~20px, LCD_TEXT_SUB) |
| 연락처 목록 | 위에서부터: 관제 → 보호자 1순위(관계 라벨 "아들" 등) → **119**. 번호는 MONO 28px+, 119는 LCD_DANGER 40px+ + soft 배경 박스 |

연락처 데이터는 대시보드 P3 대상자 연락처와 동기화(관제 번호는 P7 설정값).
**전환:** 원인 해소(배터리 회복, 재연결) 시 ①로 자동 복귀. ④ 조건 발생 시 ④가 우선.

### 12-5. 화면 ③ 복약 안내

**진입 조건:** 등록된 복약 시각 도달 → 순찰 경로 주행으로 대상자 탐색 → 사람 검출·정지 시 표시 + TTS 안내.

| 구성 | 스펙 |
|---|---|
| 헤드라인 | "{아침/점심/저녁} 약 드실 시간입니다" (32px) |
| 안내 | "불이 켜진 칸의 약을 꺼내 주세요" (20px) |
| 칸 3개 | 개방 칸: LCD_ACCENT 2px 테두리 + 칸 번호 대형 + "열림" + 실제 칸 LED 점등 연동. 완료 칸: 체크 아이콘 어둡게. 예정 칸: 번호만 어둡게 |
| 하단 | "약을 꺼내시면 자동으로 확인됩니다" (수령 감지 = 문 리드스위치 + 로드셀) |

**전환:**
- 수령 감지 → **대형 체크 애니메이션 + "확인했습니다. 건강하세요!" 2초** → ① 복귀, MED_DELIVERED 기록.
- 미수령: 안내 2회(TTS 재안내 간격은 P7 설정) 후에도 감지 없음, 또는 경로 전 구간 주행 후 대상자 미발견 → ① 복귀, 미수령 기록 + 관제 ATTENTION + 보호자 문자.

### 12-6. 화면 ④ 음성 확인

**진입 조건:** 웨어러블 심박 임계 이탈로 출동, 대상자 앞 도착 시. 음성 인식 실패에 대비해 **터치 버튼을 항상 병행 표시**한다.

| 구성 | 스펙 |
|---|---|
| 상단 | 청록 점 + "듣고 있어요 — 말씀하시거나 버튼을 눌러 주세요" (16~20px) |
| 헤드라인 | 질문 자막 "괜찮으세요?" (40px, TTS와 동기) |
| 버튼 2개 | 화면 절반 크기, 좌 `괜찮아요`(LCD_ACCENT 채움+어두운 텍스트) / 우 `도와주세요`(LCD_DANGER 채움+흰 텍스트). 각 최소 터치 타깃 120px, 텍스트 28px+ |
| 하단 | "응답이 없으면 잠시 후 담당자에게 연락합니다" — 무응답 시나리오를 사용자에게도 투명하게 예고 |

**전환 (음성·터치 동일 취급):**
- `괜찮아요` / 긍정 음성 → "다행이에요" 2초 → ① 복귀, 이벤트 해제(관제에 정상 확인 기록).
- `도와주세요` / 도움 음성 → 즉시 EMERGENCY 승격 + "담당자에게 연락하고 있어요" 표시 → 관제 통화 연결 화면(②의 긴급 변형: LCD_DANGER 테두리 + 119 최상단).
- 무응답: 1차 질문 → 대기(P7 설정 5~10초) → 2차 질문 "대답이 없으면 담당자에게 연락드릴게요" → 대기 → EMERGENCY 승격, 위와 동일.
- 불명확(소음 등): "잘 못 들었어요. 버튼을 눌러 주세요" 1회 재질문 후 무응답 흐름 합류.

---

## 13. 파일 구성 (Qt C++ / CMake)

```
careguard_dashboard/
├─ CMakeLists.txt            (Qt6::Widgets, Qt6::Svg; AUTOMOC/AUTORCC ON)
├─ src/
│  ├─ main.cpp               (QFontDatabase 폰트 등록 → QSS 로드 → MainWindow)
│  ├─ mainwindow.{h,cpp}     (TopBar + Sidebar + QStackedWidget 셸)
│  ├─ theme/
│  │  └─ tokens.h            ← 함께 제공. §1~3 상수의 단일 정의
│  ├─ widgets/               (StatusBadge, RecipientCard, KpiCard, Sparkline,
│  │                          ToggleSwitch, DayChips, StatusTableDelegate ...)
│  ├─ pages/                 (P0LoginPage, P1OverviewPage ... P7SettingsPage — 클래스당 .h/.cpp)
│  └─ dialogs/               (EventDetailDialog, RecipientDetailDialog, RobotDetailDialog,
│                             VideoPlayerDialog, MapRegisterDialog ...)
└─ resources/
   ├─ resources.qrc          (qss, fonts, icons 포함)
   ├─ careguard.qss          ← 함께 제공된 스타일시트
   ├─ fonts/  (Pretendard-*.ttf, JetBrainsMono-*.ttf)
   └─ icons/  (careguard_mark.svg ← 브랜드 심볼, lucide svg)
```

**main.cpp 초기화 순서 (고정):**
```cpp
QApplication app(argc, argv);
QFontDatabase::addApplicationFont(":/fonts/Pretendard-Regular.ttf");   // Medium, SemiBold, Bold 포함
QFontDatabase::addApplicationFont(":/fonts/JetBrainsMono-Regular.ttf");
app.setFont(QFont("Pretendard", 10));
QFile qss(":/careguard.qss");
qss.open(QFile::ReadOnly);
app.setStyleSheet(QString::fromUtf8(qss.readAll()));
MainWindow w; w.setMinimumSize(1440, 900); w.show();
```

**규칙:** 델리게이트·QPainter·그래프 등 QSS 밖의 모든 색상은 반드시 `tokens.h`의 QColor 상수를 사용한다. 소스에 HEX 리터럴 직접 작성 금지.
