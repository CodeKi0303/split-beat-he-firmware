# Split-Beat HE C86 · 최초 SWD 기록

새 C86 보드나 부팅부를 복구할 때 사용합니다. 양쪽 보드에 USB 업데이트용 부팅부가 이미 설치되어 있다면 Studio에서 앱만 갱신하세요.

## 파일 준비

`python build.py --hardware c86 --role both --scan-hz 96000 --adc-async96`로 빌드하고, 같은 옵션으로 `python tools/verify_images.py`를 실행합니다. 출력 폴더에서 왼쪽 `c86-left-factory.hex`와 오른쪽 `c86-right-factory.hex`를 각각 사용합니다. Factory HEX에는 부팅부, 앱, 앱 메타데이터가 포함됩니다.

| PCB SWD 핀 | 기능 |
|---|---|
| 1 | 3.3V 기준 전압 / VTref |
| 2 | GND |
| 3 | SWDIO |
| 4 | SWCLK |
| 5 | NRST |

PCB의 1번 핀 표시와 프로그래머의 핀 배열을 각각 확인합니다. USB로 전원을 공급할 때 프로그래머의 3.3V **전원 출력**을 병렬로 연결하지 마세요. VTref가 입력인지 장비 문서에서 확인합니다.

## 기록과 확인

1. 전원을 분리한 상태에서 왼쪽 SWD를 연결합니다. 왼쪽 PC용 포트 J401에 PC USB를 연결합니다.
2. STM32CubeProgrammer에서 ST-LINK/SWD로 연결합니다. 보드의 식별 정보와 Flash 1MiB를 확인하고 전체 Flash를 백업한 뒤 왼쪽 Factory HEX를 기록·검증합니다. 전체 지우기와 옵션 바이트 변경을 피하면 HEX에 포함되지 않은 설정 영역을 보존할 수 있습니다.
3. 왼쪽을 재시작해 USB 장치 인식을 확인합니다. 전원을 분리하고 SWD를 오른쪽으로 옮깁니다.
4. PC → 왼쪽 J401, 왼쪽 split J501 → 오른쪽 J551을 연결합니다. 왼쪽을 실행한 상태에서 오른쪽 전원 공급을 확인합니다. 오른쪽 Factory HEX를 기록·검증한 뒤 양쪽을 재시작합니다.
5. [Studio](https://studio.codekilab.com/)에서 좌우 센서 입력을 확인하고 캘리브레이션합니다. 이후 앱 업데이트는 좌우 `*.fw.json` 파일로 합니다.

오른쪽 J551은 좌우 통신·전원용입니다. 오른쪽에 전원이 들어오지 않으면 왼쪽 USB 인식과 split 케이블부터 확인하세요.
