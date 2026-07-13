# ST-Link flashing

Connect ST-Link `SWDIO`, `SWCLK`, and `GND` to STM32 `PA13`, `PA14`, and `GND`. Use the target board's regulated supply; do not connect two competing 5V supplies.

Build the firmware, then flash with OpenOCD:

```bash
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build/stm32/firmware/stm32/desktop_robot.elf verify reset exit"
```

Expected safe startup behavior:

1. All eight motor phase outputs start low.
2. W25Q CS starts high.
3. OLED shows the IDLE face if detected.
4. PC13 remains off until an ESP SPI session is healthy.
5. No motor command is accepted before a valid HELLO and mode selection.

OpenOCD is not installed automatically by this repository. On Ubuntu install it with `sudo apt-get install openocd` before hardware debugging.
