# Split-Beat HE 버전 규칙

Split-Beat HE의 모든 저장소에 적용합니다. 저장소마다 버전을 따로 올리고, 저장소 사이의 호환은 버전 번호가 아니라 펌웨어가 알려 주는 **지원 기능 비트**로 판단합니다. 기능별로 필요한 최소 버전은 [PROTOCOL.md의 호환표](PROTOCOL.md#feature-compatibility)에 있습니다.

## 이름

| 구분 | 이름 | 쓰는 곳 |
|---|---|---|
| 제품 | Split-Beat HE | 화면, 문서, 저장소 이름(`split-beat-he-*`) |
| 만든 곳 | codeki lab | 저작권 표시, 도메인 `codekilab.com` |
| 하드웨어 리비전 | C86 (다음 보드는 C87…) | 하드웨어 태그, 펌웨어 파일 이름(`c86-left-app.fw.json`), "Split-Beat HE · C86"처럼 제품명 뒤 |
| 옛 코드 이름 | C77 | 통신 규격과 코드 안 식별자만(`c77-profile-v1`, `c77_config` 등). 호환 때문에 바꾸지 않고 화면에는 쓰지 않음 |

## 번호 규칙

모두 `MAJOR.MINOR.PATCH`(Semantic Versioning)를 따릅니다.

- **PATCH**: 동작을 고치기만 하고 기능·설정·통신 형식은 그대로일 때.
- **MINOR**: 새 기능, 새 지원 기능 비트, 설정 저장 형식이나 통신 명령 추가. 이전 호스트·설정과 계속 호환될 때.
- **MAJOR**: 이전 호스트나 저장된 설정과 호환이 깨질 때. 1.0 이전에는 MINOR를 올리고 변경 기록에 명시합니다.

| 저장소 | 태그 | 현재 | 다음 기능 릴리스 | 비고 |
|---|---|---|---|---|
| split-beat-he-firmware | `vX.Y.Z` | `c86-v0.3.49` | `v0.4.0` | 0.3.x까지는 `c86-` 접두사. 펌웨어 범위의 1.0.0은 정식 USB VID/PID를 받고 설정 형식이 안정된 뒤 |
| split-beat-he-studio | `vX.Y.Z` | `v1.0.19` | `v1.1.0` | 새 탭·기능은 MINOR |
| split-beat-he-plugin | `vX.Y.Z` | `v0.1.7` | `v0.2.0` | `pyproject.toml`, `plugin.json`, `claude/manifest.json`의 버전을 함께 올림 |
| split-beat-he-hardware | 리비전 이름 (`c86`) | 없음 | `c86` | Gerber·BOM·CPL 묶음을 릴리스 첨부 파일로. 같은 리비전 안의 문서 수정은 `c86.1`처럼 |
| split-beat-he-wheel | `vX.Y.Z` | 없음 | `v1.0.0` | 끼워맞춤이 바뀌는 형상 변경은 MINOR |
| split-beat-he-docs | 없음 | — | — | main에 push하면 바로 배포 |

시험판은 `-rc.N`을 붙이고 GitHub에서 pre-release로 표시합니다(예: `v0.4.0-rc.1`). Studio 자동 배포는 정식 릴리스만 사용합니다.

## 릴리스 순서

1. 기능이 여러 저장소에 걸치면 **펌웨어 → 플러그인 → Studio** 순으로 릴리스합니다. Studio 릴리스와 펌웨어 정식 릴리스는 Studio 배포를 다시 실행합니다.
2. 새 지원 기능 비트를 추가하면 [호환표](PROTOCOL.md#feature-compatibility)에 행을 추가하고, 각 호스트의 첫 지원 버전을 채웁니다.
3. 각 저장소의 `CHANGELOG.md` 맨 위에 새 버전을 적고, 같은 내용을 GitHub 릴리스 노트로 씁니다. README에는 버전별 내용을 쌓지 않고 현재 사용법만 둡니다.
4. 릴리스 노트에는 실기 검증 여부를 적습니다(시뮬레이터 시험만 했으면 그렇다고).
