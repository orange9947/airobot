# Single Login and OLED Status Layout Design

## Scope

Fix two current usability issues:

1. Opening the web console must require one password submission, not two.
2. OLED mode and safety labels must be right-aligned at the top instead of using the expression area at the bottom.

No authentication policy, password storage, robot control protocol, expression selection, or animation timing changes are included.

## Login Design

`POST /api/v1/session/login` remains the only password verification endpoint. After successful verification it will create the existing HttpOnly session cookie and return:

- `csrf`: the existing CSRF token;
- `config`: the redacted public configuration;
- `status`: the current robot and network status.

The static login form starts in a busy state with its password field and submit button disabled. Only after `app.js` has bound the submit handler and completed the public bootstrap request may `showAuth()` enable the controls. This prevents a user on a slower ESP32 connection from submitting the native HTML form before JavaScript can call `preventDefault()`, which otherwise reloads the page and discards the first password entry.

After successful authentication, the browser renders `config` and `status` from the login response, opens the WebSocket, and hides the login layer. It does not immediately issue concurrent authenticated config and status requests, keeping the first authenticated transition atomic.

Retry after a later load failure remains available. Invalid passwords, authentication-busy responses, timeouts, and expired sessions keep their existing distinct messages. The JSON payload must never include the Wi-Fi password, API keys, password record, or session token; the session token remains confined to the existing HttpOnly `Set-Cookie` header.

## OLED Layout

The 3x5 state label will be drawn at `y = 2`, with a two-pixel right margin. Its x-coordinate is derived from the rendered text width so `AI`, `MAN`, `IDLE`, `STOP`, and `ERR` share one right edge. The link indicator remains at the top left.

All labels, including `STOP` and `ERR`, use this placement. The bottom label rendering is removed, leaving the center and bottom of the 128x64 display available for eyes, mouth, and later expression details. Safety state precedence and fixed safety faces remain unchanged.

## Data Flow

```text
password submit
  -> verify/create password record
  -> create HttpOnly session and CSRF token
  -> build redacted config and current status
  -> one login response
  -> render console and hide login layer

STM32 state/expression update
  -> clear OLED framebuffer
  -> draw safety face or animated mood face
  -> draw top-left link dot and top-right state label as status overlays
  -> flush display pages
```

## Tests

- Backend login tests assert the response contains redacted `config` and `status` and no secrets.
- Browser tests assert one password submission opens the console without navigation or a second login request.
- A delayed-script browser test asserts the static form is disabled before JavaScript binds and cannot navigate natively.
- Existing wrong-password, timeout, session-expiry, and load-retry behavior remains covered.
- Host C tests cover right-aligned label coordinates and verify the bottom expression region is not used by state labels.
- The full Python, C, browser, ESP32 bundle, and STM32 cross-build suites run before deployment.
- Hardware verification uses no motion command: confirm one web login and visually inspect label placement plus animated expressions.

## Acceptance Criteria

- One correct password submission opens the console every time the page is opened.
- The login layer does not disappear and reappear during successful authentication.
- All mode and safety labels are readable at the OLED top right.
- No state label occupies the bottom expression area.
- Existing safety behavior, credentials, provider settings, and animation remain intact.
