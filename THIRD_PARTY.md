# Dependencies and provenance

`third_party/manifest.json` lists each vendored file's SHA256, original path,
and exact repository revision. License files remain with each component.
The build verifies these hashes before compiling.

Normal builds use the committed `third_party/` files. They do not need the old
bench projects or any parent `firmware/vendor` directory. To deliberately
re-import dependencies, supply a separate directory containing the three local
Git checkouts named `weact-h723`, `cmsis-device-h7` and `stm32-usb-device` at the
revisions listed below:

```powershell
python tools/import_dependencies.py --vendor /path/to/upstream-checkouts --check-only
python tools/import_dependencies.py --vendor /path/to/upstream-checkouts
```

The importer requires this explicit path, checks the revision and tracked-file
cleanliness of every checkout, and validates selected source paths before
writing. `--check-only` leaves `third_party/` untouched. Neither command fetches
repositories, accesses a keyboard, or flashes firmware. Upstream licenses and
source hashes remain part of the imported snapshot.

| Component | Upstream / revision |
|---|---|
| STM32H7 HAL and ARM CMSIS core headers, system template | WeActStudio MiniSTM32H723 examples, `0a08d2808291a7ff38c8a4f89748693019257345` |
| STM32H723 CMSIS device header and startup | ST cmsis-device-h7, `6dac8c24d7b38ab20806d27dd7d8285a6433b8f7` |
| ST USB device core | ST stm32-mw-usb-device, `0b06460a43c30670a764cb7e58c5a7918e789f6e` |
| Python MCP SDK | `mcp==2.3.0`, official [SDK](https://github.com/modelcontextprotocol/python-sdk) |
| Python HID access | `hidapi==0.15.0`, optional hardware backend |

The HAL USB implementation is sourced from the same pinned WeAct repository's
04-SD_Test example; ADC/other HAL source from07-ADC_Test. This matches the pinned
bench dependency arrangement. Common headers are from07-ADC_Test; absent USB
headers from04-SD_Test. Firmware links CMSIS's H723-specific startup.

`rapid_trigger.c/.h` retain this workspace's tested WebHID prototype algorithm.
`settings_store.c` adapts its existing journal to a new C77 schema and record
size; old bench settings are intentionally rejected. Clock/ADC ownership logic
is adapted from the existing split-load bench. The hardware pin map, external
PHY, RS-485 direction, actual input channels, host interface and profile format
are specific to this repository. The old WeAct binary is not reused.

Documentation used for host design: [WebHID](https://developer.chrome.com/docs/capabilities/hid),
[MCP SDK](https://py.sdk.modelcontextprotocol.io/), [MCP stdio](https://py.sdk.modelcontextprotocol.io/run/).
