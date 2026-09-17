# CareGuard 상세 폴더 구조 설계 (참고용)

> 이 문서는 실제 구현 시 참고할 상세 폴더/파일 설계다.
> 현재 리포지토리에는 최상위 골격(`robot_careguard_ws/src/careguard_bringup` 등)만 생성돼 있으며,
> 아래 세부 구조는 각 기능을 실제로 구현하는 시점에 필요한 만큼 생성한다.

## robot_careguard_ws/src/ (PC에서 개발 → deploy/sync_to_limo.sh로 rsync 배포)

```
robot_careguard_ws/src/
├── careguard_bringup/          # ★ 통합 런치 (limo_base + 센서 + nav2 + 앱노드)
│   ├── launch/  careguard.launch.py / nav.launch.py / sensors.launch.py
│   ├── config/  nav2_params.yaml · amcl.yaml · costmap.yaml
│   └── maps/    home.pgm / home.yaml
├── careguard_msgs/             # 커스텀 msg/srv/action (모든 패키지 공통 의존)
│   ├── msg/  RobotState.msg · MedEvent.msg · IntruderAlert.msg
│   └── action/  DeliverMedication.action · Patrol.action
├── careguard_nav/              # 주행 계층
│   ├── initial_pose_setter.py  # ★ 충전 웨이포인트를 초기위치로 자동 발행
│   ├── waypoint_manager.py     # 웨이포인트 CRUD (서버에서 내려받음)
│   └── dock_return.py          # 홈(충전소) 복귀
├── careguard_mission/          # 시나리오 실행 엔진
│   ├── medication_task.py      # 복약 시간 → 이동 → 약통 개방 → 3분 대기 → 복귀
│   ├── patrol_task.py          # 순찰 스케줄
│   └── mission_scheduler.py    # cron형 스케줄러 (서버 시간표 동기화)
├── careguard_perception/
│   ├── intruder_detector.py    # YOLO person + 미등록자 판정 → 스냅샷
│   └── pill_taken_checker.py   # 약 반출 확인 (카메라 or 리드/포토 센서)
├── careguard_hmi/               # LCD 표정 · LED · 스피커 · 통화
│   ├── face_display.py         # 상황별 표정 (limo_lcd_screens_wireframe 기준)
│   ├── audio_player.py         # 등록 영상/음성 재생, 복약 안내
│   └── contact_display.py      # 호출 제한 시 관제 연락망 노출
├── careguard_health/           # 스마트워치/링 심박 수신 → 이상 시 SOS
│   └── vital_monitor.py
├── careguard_hw/                # 약통 서보·센서 (아두이노 시리얼)
│   ├── pillbox_controller.py
│   └── firmware/pillbox/pillbox.ino
└── careguard_agent/             # ★ ROS2 ↔ FastAPI 브리지 (유일한 외부 통신구)
    ├── uplink_node.py          # 상태·이벤트·스냅샷 업로드
    ├── downlink_node.py        # 명령·스케줄·영상 수신 → ROS2 토픽/액션
    └── media_uploader.py       # 영상/사진 업로드
```

## pc_careguard/ (PC에서 실행: server + dashboard)

```
pc_careguard/
├── server/                     # FastAPI 중앙서버
│   ├── app/
│   │   ├── main.py
│   │   ├── api/v1/   robots.py · recipients.py · events.py · medication.py
│   │   │              patrol.py · media.py · settings.py
│   │   ├── ws/       robot_ws.py (로봇↔서버) · dashboard_ws.py (서버↔Qt)
│   │   ├── models/   SQLAlchemy ORM
│   │   ├── schemas/  Pydantic
│   │   ├── services/ notifier.py (보호자 이메일 알림) · scheduler.py
│   │   └── core/     config.py · security.py · db.py
│   ├── alembic/                # 마이그레이션
│   ├── storage/                # 영상·스냅샷 파일 (DB엔 경로만, git 커밋 금지)
│   └── requirements.txt
├── dashboard/                  # Qt C++ 대시보드
│   ├── src/
│   │   ├── main.cpp
│   │   ├── net/      ApiClient · WsClient      # REST + WebSocket
│   │   ├── model/    RobotModel · EventModel · RecipientModel
│   │   ├── view/     P1FleetOverview · P2EventQueue · P3Recipient
│   │   │             P4Video · P5SlamMapping · P6PatrolRoute · P7Settings
│   │   └── widget/   공통 컴포넌트 (와이어프레임 기준)
│   ├── resources/    qss/ (CareGuard_DESIGN_SYSTEM 토큰) · icons/ · fonts/
│   └── CMakeLists.txt
└── db/
    ├── schema/careguard_schema.sql       # 항상 최신 상태인 전체 스키마 1개 파일.
    │                                      # docker-entrypoint-initdb.d가 이 폴더만 마운트
    └── migrations/                       # 번호순 이력. 자동 실행 안 됨(수동 psql 적용용)
        ├── 00_alter_database_timezone.sql
        ├── 01_create_base_schema.sql
        └── 02_..N_alter_*.sql
```

## docs/ 및 deploy/

```
docs/                       # 시나리오서 · 요구사항 · 아키텍처
deploy/
├── docker-compose.yml      # postgres + fastapi
└── sync_to_limo.sh         # PC → LIMO rsync 배포
```

## 참고

- `shared/api_contract/`: 로봇 ↔ 서버 ↔ 대시보드가 공통으로 참조하는 OpenAPI 스펙, 이벤트 코드 정의는 최상위 설계(`~/DYNC_CareGuard/` README 참고)와 별도로 유지한다.
- 실제 구현 착수 시, 해당 기능 담당 패키지/모듈만 이 문서를 참고해 생성한다. 전체를 한 번에 스캐폴딩하지 않는다.
