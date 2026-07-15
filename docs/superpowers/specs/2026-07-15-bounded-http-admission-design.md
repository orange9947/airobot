# Bounded HTTP Admission Queue Design

## Problem

The phone loads the new inline startup probe but reports `控制程序加载失败`. Opening the same versioned `app.js` URL directly succeeds. The root document contains roughly 24 icon requests, while the ESP32 web service processes at most four HTTP connections and immediately returns HTTP 503 to every additional connection. Because `app.js` is discovered near the end of the document, it is consistently among the rejected subresource requests.

Raising the processing limit would increase simultaneous file buffers on MicroPython. Moving or inlining the script would avoid only this one failed resource and leave the server behavior unchanged for future modules.

## Selected Approach

Keep `MAX_HTTP_CLIENTS` at four and add a bounded admission wait before reading a request:

- up to four connections may actively execute the existing request path;
- up to four additional connections may wait for an active slot;
- a waiting connection polls cooperatively and enters as soon as a slot is released;
- a connection that waits for 3000 milliseconds, or arrives when all four wait positions are occupied, receives the existing HTTP 503 response;
- the HTTP request is not parsed or logged while it is waiting.

The production admission timeout defaults to 3000 milliseconds. The constructor accepts an override so host tests can exercise timeout behavior without a three-second delay.

The wait is intentionally bounded in both count and time. A normal mobile browser, which commonly opens more than four same-origin connections during initial page loading, can drain its short burst without increasing the number of simultaneously processed files.

## Lifecycle and Counters

`WebService` tracks active and waiting counts separately. A small admission method owns all transitions:

1. claim an active slot immediately when one is available;
2. otherwise claim a wait position if the wait queue is not full;
3. release the wait position before claiming the newly available active slot;
4. release the wait position on timeout, cancellation, or error.

`_handle_client` uses one final cleanup path. It decrements the active count only when admission succeeded, and it always closes the writer. Cooperative scheduling makes the availability check and active-count increment atomic because there is no `await` between them.

Strict FIFO ordering is not required; every admitted waiter uses the same 3000-millisecond bound.

## Error Handling and Scope

Queue-full and admission-timeout cases retain the generic `503 Service Unavailable` response and do not expose connection details. Existing request deadlines, body and header limits, authentication, CSRF, WebSocket limits, resource upload, STM32 communication, display behavior, and motion safety remain unchanged.

No client-side retry loop is added. The existing startup diagnostic remains useful if a genuine script transfer error occurs.

## Verification

Host tests will cover:

- four blocked active clients plus a fifth request that waits, receives a released slot, and completes normally;
- a waiting request that reaches an injected short admission limit and receives HTTP 503, while the production default remains 3000 milliseconds;
- a ninth request that is rejected when four active and four waiting positions are occupied;
- active and waiting counters returning to zero after success, timeout, cancellation, and request-read timeout;
- the existing whole-request deadline and overload response contracts;
- the complete Python suite, JavaScript syntax check, and ESP32 bundle build.

Hardware acceptance preserves `/config` and sends no motion command. After a notified ESP32 reset, the phone opens a cache-busted root URL and must progress from the startup probe to the enabled password form. The existing password must log in successfully, and the console must still report STM32 status.
