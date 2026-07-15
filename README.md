# AI Desktop Robot

Modular desktop robot firmware for an ESP32-S3 and STM32F103C8T6 pair.

- ESP32-S3: MicroPython, Wi-Fi, local web UI, OpenAI/DeepSeek adapters, high-level tools.
- STM32F103C8T6: SPI mailbox, safety state machine, two 28BYJ-48 motors, SSD1306, EC11, W25Q.
- Link: ESP32 SPI master on GPIO10-13 to STM32 SPI1 slave on PA4-PA7.

The STM32 is authoritative for motion and safety. Model output never reaches hardware without local validation.

## Host tests

```bash
python3 protocol/generate.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -S . -B build/host -DROBOT_BUILD_HOST_TESTS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

## STM32 build

Initialize the pinned STM32CubeF1 dependency, then configure with an ARM GCC toolchain:

```bash
./tools/bootstrap_stm32cube.sh
export AIROBOT_ARM_GCC_ROOT="$HOME/.cache/airobot-toolchain/root"
cmake -S . -B build/stm32 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
  -DROBOT_BUILD_HOST_TESTS=OFF \
  -DROBOT_BUILD_STM32=ON
cmake --build build/stm32
python3 tools/check_firmware_size.py build/stm32/firmware/stm32/desktop_robot.elf
```

The build produces `desktop_robot.elf`, `.hex`, `.bin`, and a linker map under `build/stm32/firmware/stm32/`.

## ESP32-S3 deploy

Flash the stable MicroPython v1.28.0 `ESP32_GENERIC_S3-SPIRAM_OCT` build, then deploy the application:

```bash
python3 -m pip install --user esptool mpremote
python3 tools/deploy_esp32.py --port /dev/ttyACM0
```

See [ESP32 setup](docs/esp32/setup.md) for flashing, first boot, provider configuration, and hardware bring-up.
The web console can be previewed without hardware by serving `web/` and opening `/?demo=1`.

## OLED expression resources

Install the host-only image dependency, regenerate the sample PNGs, and build a validated package:

```bash
python3 -m pip install -r requirements-tools.txt
python3 tools/generate_sample_faces.py
python3 tools/resource_pack.py build \
  examples/resources/default_faces/manifest.json \
  build/default-faces.arp
python3 tools/resource_pack.py verify build/default-faces.arp
```

The sample contains two weighted clips for each of the six ordinary expressions. Uploads use the authenticated local API and prompt for the administrator password without placing it on the command line:

```bash
python3 tools/upload_resources.py upload http://ROBOT_IP build/default-faces.arp
python3 tools/upload_resources.py status http://ROBOT_IP
```

The current console uses plain HTTP, so the administrator password and session are not encrypted on the network. Only use resource upload on the robot's isolated access point or another trusted private LAN; do not expose port 80 to the Internet.

Before the first hardware upload, disconnect the motor 5 V rail and verify that the STM32 and W25Q 3.3 V supply is stable. A resource update never replaces the active Bank until the new package and commit marker have been verified.

## Documents

- [System design](docs/superpowers/specs/2026-07-14-desktop-robot-design.md)
- [Implementation plan](docs/superpowers/plans/2026-07-14-m0-m1-foundation-stm32-plan.md)
- [Final wiring](docs/hardware/wiring-v1.md)
- [ST-Link flashing](docs/hardware/stlink-flashing.md)
- [ESP32-S3 setup](docs/esp32/setup.md)

## Safety

The EC11 long press is a software stop, not a certified emergency stop. Test with wheels raised and add a normally-closed hardware switch that cuts motor power before operating the robot on a surface.
