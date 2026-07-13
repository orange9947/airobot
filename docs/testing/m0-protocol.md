# M0 SPI Mailbox Protocol

The ESP32 is the SPI master and the STM32 is the SPI1 slave. Every CS assertion exchanges one fixed 268-byte slot. The response returned during a transaction was prepared after an earlier transaction, so clients must poll for matching ACK and event slots.

## Simulator

```bash
python3 tools/stm_simulator.py --tcp 127.0.0.1:9001
python3 tools/protocol_console.py --tcp 127.0.0.1:9001 hello
```

The TCP stream preserves the same slot boundary by reading and writing exactly 268 bytes per transaction.
