# Split-Beat HE · Firmware

C86 스플릿 홀 이펙트 컨트롤러용 STM32 펌웨어입니다. 좌우 앱과 SWD 초기 설치용 부팅부, 센서 캘리브레이션·Rapid Trigger·매핑을 제어하는 로컬 MCP 도구가 포함되어 있습니다.

## 사용

- 새 보드는 [최초 SWD 기록](C86-FIRST-FLASH.md)을 진행합니다. 이미 부팅부가 설치되어 있으면 [릴리스](https://github.com/CodeKi0303/split-beat-he-firmware/releases)의 좌우 `*.fw.json`을 [Studio](https://studio.codekilab.com/)의 펌웨어 화면에서 설치합니다.
- PC는 왼쪽 PC 포트에 연결합니다. 오른쪽은 왼쪽 split 포트로 연결하며, 업데이트도 왼쪽을 통해 중계합니다.
- [로컬 MCP 설치와 보정](docs/MCP-CONTROL.md), [USB 업데이트 프로토콜](docs/FIRMWARE-UPDATE.md), [하드웨어 제작](https://github.com/CodeKi0303/split-beat-he-hardware), [Studio 소스](https://github.com/CodeKi0303/split-beat-he-studio).

## 기본 매핑

v0.3.42부터 새 설정의 28개 입력 매핑은 [default-bindings.json](default-bindings.json)을 기준으로 합니다. Studio와 플러그인도 같은 기준을 사용하며, 물리 입력 ID와 HID 매핑은 별도로 유지합니다.

이 기본값은 유효한 저장 설정이 없는 새 보드에 적용됩니다. 펌웨어 업데이트는 기존에 저장한 매핑·보정·RT 설정을 덮어쓰지 않습니다. 기준 파일에는 개인 보정값이나 장치 정보가 없으며, 새 보드는 캘리브레이션 후 출력을 켜야 합니다.

## 빌드와 검증

Python과 ARM GNU Toolchain을 준비합니다. 툴체인이 PATH에 없으면 `--toolchain`에 `bin` 디렉터리를 전달합니다.

```sh
python build.py --hardware c86 --role both --scan-hz 96000 --adc-async96
python tools/verify_images.py --hardware c86 --role both --scan-hz 96000 --adc-async96
python -m pip install -r host/requirements-lock.txt
python tools/check_all.py --firmware
```

동일 디렉터리에 Studio도 클론했다면 `python tools/check_all.py --studio ../split-beat-he-studio`로 C·Python·JavaScript의 프로토콜과 Hall 곡선 일치까지 확인할 수 있습니다. 검사는 오프라인이며 하드웨어 기록을 실행하지 않습니다.

릴리스에는 동일한 버전의 좌우 앱을 첨부합니다. 한쪽 수집 방식만 수정해도 다른 쪽 버전과 패키지를 함께 맞추며, 오프라인 검사와 실기 검증 범위는 릴리스 안내에 구분해 기록합니다. `main`에 포함된 커밋의 릴리스가 최신으로 게시되면 Studio에 배포 요청을 전달합니다. 요청용 Secret은 `STUDIO_DISPATCH_TOKEN`이며 Studio 저장소의 Actions 쓰기 권한만 필요합니다.

직접 작성한 코드는 [MIT](LICENSE)입니다. 번들 라이브러리별 라이선스와 출처는 [THIRD_PARTY.md](THIRD_PARTY.md)를 참조하세요.
