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

## Documents

- [System design](docs/superpowers/specs/2026-07-14-desktop-robot-design.md)
- [Implementation plan](docs/superpowers/plans/2026-07-14-m0-m1-foundation-stm32-plan.md)

## Safety

The EC11 long press is a software stop, not a certified emergency stop. Test with wheels raised and add a normally-closed hardware switch that cuts motor power before operating the robot on a surface.
