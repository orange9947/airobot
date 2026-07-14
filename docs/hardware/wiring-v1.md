# Wiring v1

All logic is 3.3V. All boards share ground. Motors use the regulated 5V rail through their ULN2003 boards.

## ESP32-S3 to STM32 SPI mailbox

ESP32-S3 is the SPI master. STM32 SPI1 is the slave.

| ESP32-S3 | Signal | STM32F103C8T6 |
| --- | --- | --- |
| GPIO10 | CS / NSS | PA4 |
| GPIO11 | MOSI | PA7 |
| GPIO12 | SCK | PA5 |
| GPIO13 | MISO | PA6 |
| GND | Ground | GND |

Initial bus settings: Mode 0, MSB first, 1 MHz, fixed 268-byte transaction.

## STM32 peripherals

| Peripheral | Signal | Pin |
| --- | --- | --- |
| W25Q | CS | PB12 |
| W25Q | SCK / CLK | PB13 |
| W25Q | MISO / DO | PB14 |
| W25Q | MOSI / DI | PB15 |
| OLED | SDA | PB6 |
| OLED | SCL | PB7 |
| EC11 | VCC | 3.3V |
| EC11 | GND | GND |
| EC11 | A | PA0 |
| EC11 | B | PA1 |
| EC11 | Common C | GND |
| Reserved button | SW | PA2 (not fitted) |
| Left ULN2003 | IN1-IN4 | PA8, PA9, PA10, PA11 |
| Right ULN2003 | IN1-IN4 | PB0, PB1, PB10, PB11 |
| ST-Link | SWDIO | PA13 |
| ST-Link | SWCLK | PA14 |

PB6 is intentionally SDA and PB7 is intentionally SCL. The firmware uses software I2C because this is the reverse of the STM32F1 hardware I2C1 default mapping.

The current EC11 has no push switch. Each detent changes mode immediately. PA2 remains pulled up and ignored until a separate button is fitted and `BOARD_ENCODER_BUTTON_PRESENT` is enabled.

## Power

- Verify the finished 18650 boost pack can sustain 5V/3A.
- Feed ESP32 VIN, STM32 5V, and both ULN2003 motor VCC inputs from separate branches.
- Place at least 470 uF near each ULN2003 board and 100 uF near the logic branch.
- Power OLED and W25Q from 3.3V.
- Never connect the 5V rail to an MCU GPIO.
- Keep wheels raised during first motor tests.
