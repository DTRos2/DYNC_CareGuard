# DYNC_CareGuard
2차 메인프로젝트 - 노인돌봄서비스

## 리포지토리 구조

```
DYNC_CareGuard/                    # git repo 루트 (개발은 PC에서 전체를 checkout)
├── robot_careguard_ws/            # ROS2 워크스페이스 → deploy/sync_to_limo.sh로 LIMO에 rsync 배포
│   └── src/  careguard_bringup · careguard_msgs · careguard_nav ...
├── pc_careguard/                  # PC에서 실행 (server + dashboard)
│   ├── server/                    # FastAPI
│   ├── dashboard/                 # Qt C++
│   └── db/
├── shared/
│   └── api_contract/              # OpenAPI 스펙 · 이벤트 코드 정의 (양쪽 참조)
├── docs/                          # 시나리오서 · 와이어프레임 · PDF · 상세 설계
├── deploy/                        # sync_to_limo.sh · docker-compose.yml
├── scripts/
│   └── setup-git-hooks.sh         # .githooks/ 활성화 (core.hooksPath 설정)
├── .githooks/                     # commit-msg · pre-commit (COMMIT_RULES.md 강제)
├── COMMIT_RULES.md                # 브랜치 전략 · 커밋 메시지 · PR 규칙
├── CONVENTIONS.md                 # 네이밍 · 리포 구조 · 파일 관리 규칙
├── .gitignore
└── .gitattributes                 # LFS 규칙 (*.stl, *.pgm)
```

- **개발/배포 방식**: LIMO에서 별도로 git checkout(sparse-checkout)하지 않는다. 전체 리포는 PC에서 개발하고, `robot_careguard_ws/`만 `deploy/sync_to_limo.sh`(rsync)로 LIMO에 밀어넣어 실행한다. LIMO는 git 클라이언트 없이 rsync 수신 대상 역할만 한다.
- **robot_careguard_ws/**: ROS2 워크스페이스. PC에서 작성하고 rsync로 LIMO에 배포
- **pc_careguard/**: PC(서버 + 대시보드)에서 실행
- **shared/api_contract/**: 로봇 ↔ 서버 ↔ 대시보드가 공통으로 참조하는 API/이벤트 코드 정의
- **.githooks/**: 클론 후 `./scripts/setup-git-hooks.sh` 한 번 실행해야 활성화됨. 자세한 내용은 [`COMMIT_RULES.md`](COMMIT_RULES.md) 5절 참고

각 디렉토리 하위의 상세 패키지/파일 설계는 [`docs/FOLDER_STRUCTURE.md`](docs/FOLDER_STRUCTURE.md) 참고. 실제 구현은 필요한 시점에 해당 부분만 생성한다. 브랜치·커밋·PR 규칙은 [`COMMIT_RULES.md`](COMMIT_RULES.md), 네이밍·파일 관리 규칙은 [`CONVENTIONS.md`](CONVENTIONS.md) 참고.
