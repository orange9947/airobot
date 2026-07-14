# SPI Safety Session Recovery Design

## Problem

STM32 starts heartbeat supervision only after `HELLO_REQ`. If STM32 resets after ESP32 has already established a connection, or if a heartbeat timeout closes the STM32 session, ESP32 can continue receiving valid NOOP and state frames and incorrectly remain `connected`.

Commands then reach STM32 while `session_active` and `link_healthy` are false. Remote STOP still enters ESTOP with fault code 11, but confirmed `CLEAR_ESTOP` is rejected with `BAD_STATE` (error 5). More importantly, motion commands could run while heartbeat supervision remains inactive after an STM32-only reset.

## Safety Invariants

1. A valid SPI slot may restore communication health and heartbeat supervision.
2. Restoring communication must never clear ESTOP, clear a fault code, start motion, or restore a previous mode.
3. ESTOP may return to IDLE only after an explicit operator confirmation, a healthy link, and no active motion.
4. Invalid CRC, protocol version, length, or framing must never restore the session.
5. Link timeout must continue to stop motion and latch ESTOP.

## Design

`safety_supervisor_valid_slot()` will start or refresh the safety session for every slot that has already passed the protocol decoder. It will set:

- `session_active = true`;
- `link_healthy = true`;
- `last_valid_slot_ms = now_ms`.

The current `route_slot()` order already calls this function only after `ROBOT_SLOT_OK`, before dispatching the decoded message. HELLO remains available for protocol version and capability negotiation, but it is no longer the only event that can arm heartbeat supervision.

When a timeout occurs, the supervisor still sets the session inactive, marks the link unhealthy, stops motion, and latches ESTOP. The next valid heartbeat, NOOP, state request, or command restores only the session fields. `robot_state_t` remains ESTOP with its existing fault code until `CLEAR_ESTOP` passes its current checks.

## State Flow

```text
boot or heartbeat timeout
  -> session inactive / link unhealthy
  -> motion off / ESTOP latched when timeout caused it
  -> receive slot
  -> CRC, version, length, and framing validation
  -> invalid: keep session inactive
  -> valid: session active / link healthy / watchdog timestamp refreshed
  -> keep ESTOP and fault code unchanged
  -> operator confirms CLEAR_ESTOP
  -> require ESTOP + healthy link + no active motion
  -> return to IDLE and clear fault code
```

## Components

### STM32 Safety Supervisor

Change the existing valid-slot function rather than adding a second recovery API. Session start and refresh then share one implementation and one timestamp rule.

### STM32 Application

No command handler bypass is added. `CLEAR_ESTOP` continues to call `robot_state_clear_estop()` with the current safety conditions. Motion and mode handlers remain unchanged.

### ESP32 and Protocol

No message layout or ESP32 API change is required. The wired ESP32 remains the only SPI master, and every transmitted slot already uses the generated protocol framing and CRC.

### Simulator

The STM32 simulator will mirror the firmware: any successfully decoded inbound slot reactivates its session and heartbeat timestamp without changing ESTOP. This prevents host tests from accepting behavior that differs from hardware.

## Error Handling

- Malformed slots increment existing receive errors and do not call the safety recovery path.
- A valid slot in ESTOP restores link health but leaves ESTOP visible.
- `CLEAR_ESTOP` still returns `BAD_STATE` if the robot is not in ESTOP or motion is active.
- No automatic retry of a rejected safety command is introduced.

## Tests

- Unit test: a valid slot on a freshly initialized supervisor starts the session.
- Unit test: timeout latches ESTOP and disables the session.
- Unit test: a later valid slot restores link health without changing ESTOP or its fault code.
- Unit test: operator-confirmed clear then returns to IDLE.
- Simulator test: timeout, valid traffic, and clear follow the same sequence.
- Regression tests: invalid traffic cannot restore the session; remote STOP and expression rejection remain unchanged.
- Hardware test without motion: start ESP32, reset STM32, issue remote STOP, confirm web clear succeeds and returns IDLE with fault code 0.

## Acceptance Criteria

- Web-confirmed ESTOP recovery no longer returns error 5 after an STM32-only reset.
- Heartbeat supervision is active after any valid ESP32 traffic, even if HELLO occurred before STM32 reset.
- Communication recovery alone never clears ESTOP.
- No motor movement occurs during recovery or hardware verification.
