# ESP32-S3 N16R8 setup

This project targets `YD-ESP32-23-2022-V1.3`, with 16 MiB flash and 8 MiB octal PSRAM.

## 1. Install host tools

```bash
python3 -m pip install --user esptool mpremote
```

Find the serial port after connecting USB:

```bash
mpremote connect list
```

## 2. Flash MicroPython

Use the stable `ESP32_GENERIC_S3-SPIRAM_OCT` v1.28.0 `.bin` image from the
[official ESP32-S3 download page](https://micropython.org/download/ESP32_GENERIC_S3/).
Do not use the generic non-octal image for this N16R8 board.

Hold `BOOT`, tap `RST`, then release `BOOT`. Replace the port and image filename below:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write-flash -z 0 \
  ESP32_GENERIC_S3-SPIRAM_OCT-20260406-v1.28.0.bin
```

Verify the interpreter and PSRAM-backed heap:

```bash
mpremote connect /dev/ttyACM0 exec "import sys,gc; print(sys.implementation); print(gc.mem_free())"
```

## 3. Deploy the application

```bash
python3 tools/deploy_esp32.py --port /dev/ttyACM0
mpremote connect /dev/ttyACM0 repl
```

Deployment replaces application files but deliberately preserves `/config`, which contains Wi-Fi,
the password verifier, and provider secrets. Run `python3 tools/build_esp_bundle.py` to inspect the
exact device filesystem payload without connecting hardware.

## 4. First boot

With no saved Wi-Fi, USB serial prints a protected setup network similar to:

```text
Setup Wi-Fi: Robot-A1B2 / RBT-1234ABCD / http://192.168.4.1
```

Connect to that network, open the printed address, and set an admin password of at least eight
characters. In Settings, enter Wi-Fi and either an OpenAI or DeepSeek API key. API keys and the
Wi-Fi password are write-only in the web UI and are never returned by its API. Power-cycle the
ESP32 after changing Wi-Fi.

For OpenAI, set a model that supports function tools. For DeepSeek, the default is
`deepseek-chat`. Switching provider does not change the robot tool schema or STM32 protocol.

## 5. Bring-up order

1. Keep both wheels raised and leave both ULN2003 motor supplies disconnected.
2. Flash and power the STM32; verify the OLED reaches `IDLE`.
3. Wire the shared ground and SPI link, then power the ESP32.
4. Confirm the web console reports `ESP <-> STM` online, the W25Q capacity, and no increasing SPI receive errors.
5. Select manual mode and test one 100 ms movement at low speed.
6. Connect motor 5 V, test direction, then test the 750 ms communication-loss stop.

The EC11 long press and web stop are software stops. A normally-closed physical switch that cuts
the motor power branch is still required for a dependable hardware emergency stop.
