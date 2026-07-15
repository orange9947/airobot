# Mobile HTTP Header Compatibility Design

## Problem

The ESP32 web server decodes each complete HTTP header line as ASCII. HTTP field names are ASCII tokens, but field values may contain bytes in the `0x80..0xFF` range. A mobile browser that sends such a value causes every static or API request to fail with `invalid HTTP header`, leaving the login page at its initial connecting state.

## Selected approach

Parse each header line as bytes and split it at the first colon. Decode and validate the field name as strict ASCII, then decode the field value as Latin-1 so every allowed header byte has a one-to-one representation. Validate control characters from the raw value before storing it.

The parser keeps all current limits and request-smuggling defenses:

- request line, header line, total header bytes, header count, and body size remain bounded;
- field names remain limited to lowercase letters, digits, and hyphens after normalization;
- empty, malformed, non-ASCII, and duplicate field names remain rejected;
- NUL and other control bytes remain rejected except horizontal tab;
- `Transfer-Encoding` remains unsupported and duplicate `Content-Length` remains rejected.

The authentication, CSRF, session, resource upload, WebSocket, and motion paths do not change.

## Error handling

Malformed field names or disallowed control bytes return HTTP 400 without echoing the offending header. Oversized headers continue to return HTTP 431. Valid high-byte field values are accepted but are not logged or returned to the client.

## Verification

Host tests will cover a mobile-style high-byte field value, a non-ASCII field name, a control byte, duplicate names, and the existing size/count limits. The full Python suite and ESP32 bundle build must pass.

Hardware acceptance uses the existing phone and robot network:

1. deploy the updated ESP32 bundle while preserving `/config`;
2. open `/api/v1/bootstrap` and confirm it returns JSON;
3. open the root page and confirm the password input becomes available;
4. confirm STM32 communication remains healthy and no motion command is sent.
