# Pico Media FS

Pico Media FS is a bare-metal media browser and display project for the Raspberry Pi Pico (RP2040). It is being developed to read media from a FAT32 SD card and present it through an ILI9341 TFT display. The current code is a hardware bring-up and streaming prototype; the complete browser and media player are planned work.

## Project Features

- Browse files and folders on a FAT32-formatted SD card.
- Use physical push buttons to navigate media folders and select files.
- Build a clear, navigable on-screen interface for media selection and playback.
- Display common image formats, including JPEG/JPG and PNG.
- Play MP3 audio and, as a longer-term goal, video.
- Keep the firmware small enough for the RP2040 by streaming data where possible and using DMA for high-throughput transfers.
- Build a DMA-driven media pipeline that moves data from the SD card through file handling and media decoding to the display or audio output, while keeping the UI responsive and working within the RP2040’s memory and bandwidth limits.

## Current state

The firmware currently initializes the display and SD card, reads SD blocks over SPI, parses an MBR partition entry and FAT32 BPB, walks and prints the directory tree over UART, and streams one hard-coded 240 x 320 RGB565 image file (`Prince.rgb`) from the card to the display. The image path is a raw pixel stream, not a JPEG or PNG decoder. The application then blinks the onboard LED.

Physical push-button navigation is included for browsing media folders and selecting files. The complete on-screen file browser and playback interface are still in progress. JPEG/JPG and PNG decoding, MP3 playback, video playback, and DMA-based transfers are planned and are not implemented in the current firmware.

## Hardware

The source targets a Raspberry Pi Pico with an RP2040, an ILI9341 240 x 320 TFT, and an SPI SD card module. The current source configures these GPIOs:

| Device | Signal | Pico GPIO |
| --- | --- | ---: |
| SD card, SPI1 | SCK | GP10 |
| SD card, SPI1 | MOSI | GP11 |
| SD card, SPI1 | MISO | GP12 |
| SD card | CS | GP13 |
| ILI9341, SPI0 | MISO | GP16 |
| ILI9341, SPI0 | CS | GP17 |
| ILI9341, SPI0 | SCK | GP18 |
| ILI9341, SPI0 | MOSI | GP19 |
| ILI9341 | D/C | GP20 |
| ILI9341 | RESET | GP21 |

Connect module power, ground, and any required backlight pin according to the module specifications. Confirm logic levels and power requirements for the specific SD and display modules before connecting them. The display MISO line is configured in the source, though the current display operations are write-oriented.

## Repository layout

```text
.
|-- .gitignore
|-- .vscode/                 # Parent-level editor settings
|-- README.md
`-- Pico_Media_FS/           # Firmware project and Pico SDK CMake files
    |-- CMakeLists.txt
    |-- Pico_Media_FS.c      # Startup, device bring-up, and main loop
    |-- bitmap.c / bitmap.h  # Built-in font bitmaps
    |-- fat32.c / fat32.h    # MBR, FAT32 geometry, directory and file reads
    |-- ili9341.c / .h       # TFT initialization and drawing primitives
    |-- pico_sdk_import.cmake
    |-- sdcard.c / .h        # SD-over-SPI initialization and block reads
    |-- timer.c / .h         # Timing helpers
    `-- uart.c / .h          # UART output and input helpers
```

The firmware project has its own `.gitignore` for generated build files. The repository-level `.gitignore` covers generated CMake and firmware outputs and VS Code settings; the parent `.vscode` directory can be force-added if it is intentionally part of the repository.

## Build

### Requirements

- Raspberry Pi Pico SDK. `CMakeLists.txt` currently selects SDK version `2.3.1` for the Pico VS Code extension integration.
- CMake and a supported ARM embedded toolchain for RP2040, or the Raspberry Pi Pico VS Code extension with its configured toolchain.
- Raspberry Pi Pico (RP2040) as the selected board.

### Configure and compile

From the firmware directory, configure a build directory and compile:

```sh
cd Pico_Media_FS
cmake -S . -B build
cmake --build build
```

The CMake target is `Pico_Media_FS`. Pico SDK extra outputs are enabled, so the build produces the firmware formats supported by the configured SDK/toolchain, commonly including `.uf2` for board deployment. If CMake cannot locate the SDK, configure `PICO_SDK_PATH` according to the Pico SDK setup instructions or use the VS Code extension setup.

Copy the generated UF2 file to the Pico while it is in BOOTSEL mode. The exact output path depends on the selected generator and build configuration.

## SD card and current demo data

The current startup path expects an SD card accessible through the configured SPI1 pins. It reads block 0 as an MBR, uses the partition start to read the volume boot record, parses FAT32 geometry, and lists the root directory and its subdirectories through UART.

The image demonstration is intentionally specific to the current test setup. It expects a file named `Prince.rgb` represented by a fixed starting cluster and byte count in `fat32.c`; the current display path streams 153,600 bytes, corresponding to 240 x 320 pixels at 16 bits per pixel (RGB565). That file layout and cluster value are not discovered from a general file selection workflow. To use the demo, prepare the SD card and raw RGB565 data to match those assumptions, or adapt the code to locate the file through its directory entry.

The firmware currently reports bring-up and filesystem diagnostics through UART routines. The CMake configuration disables Pico SDK stdio UART and USB, so the custom UART implementation and wiring/configuration must be considered when observing logs.

## UART0 debugging

The firmware sends diagnostic output through its custom UART0 transmitter. Connect a USB-to-TTL serial adapter to the Pico as follows:

| USB-to-TTL adapter | Raspberry Pi Pico |
| --- | --- |
| RXD | GP0 (UART0 TX) |
| GND | GND |

Use a 3.3 V UART-compatible adapter and configure the terminal for **115200 baud, 8 data bits, no parity, 1 stop bit (8N1)**. The firmware currently enables UART0 transmit only, so the adapter's TXD pin is not needed. Power the Pico separately over USB; do not connect an adapter's 5 V output to the Pico.

## Planned development

1. **Filesystem and file selection** - replace hard-coded file metadata with directory-entry-based file reads; add robust navigation, selection, and error reporting.
2. **User interface** — create a consistent screen layout with directory breadcrumbs, file lists, selection state, media type indicators, playback controls, progress, and status/error views; connect the push-button navigation to these views.
3. **Image display** - add JPEG/JPG and PNG parsing/decoding, dimension and format validation, scaling or cropping to the panel, and a streaming decode path that respects RP2040 RAM limits.
4. **Audio** - add MP3 frame parsing/decoding, an audio output path and controls, and playback timing/status in the interface.
5. **Video** - investigate practical container and codec limits for RP2040, then define a constrained playback target. Video requires coordinated storage reads, decoding, frame output, and audio synchronization; it is a longer-term goal.
6. **DMA and performance** - evaluate DMA for SD SPI transfers and display SPI writes, including buffer ownership, synchronization, error handling, and throughput measurements. DMA is planned; current transfers are blocking/polled.
7. **Reliability** - test invalid media, fragmented files, unsupported FAT variants, card read errors, and recovery paths on hardware.

Formats and performance targets should be selected against measured memory use and playback speed on the target board. Listing a format as a goal does not imply support for every profile, resolution, or encoding option.

## Design notes

- The SD card and display use separate SPI peripherals in the current pin configuration.
- FAT32 file reads are structured around a consumer callback so file bytes can be processed in blocks instead of requiring a whole file in RAM.
- The current raw-image consumer writes RGB565 bytes directly to the ILI9341 after setting a full-screen address window.
- DMA, an interactive UI, image decoders, audio output, and video decoding will need explicit memory and throughput budgets before implementation.

## Limitations

- Hardware behavior depends on the exact Pico, SD card module, display module, wiring, and SD card format.
- The existing demo is not yet a general media player and does not provide a complete UI or user input flow.
- Only the raw RGB565 image path is present; JPEG/JPG, PNG, MP3, and video playback are not currently available.
- The FAT32 code is an in-progress embedded implementation and should not be treated as a general-purpose or write-capable filesystem driver.

## Contributing

Keep hardware-specific behavior explicit, document tested board and module combinations, and mark planned features separately from working features. When adding media support, include format limits, RAM requirements, and measured performance on the target hardware.
