# Web Startup Diagnostics and Asset Versioning Design

## Problem

The phone can open both `/api/v1/bootstrap` and `/app.js` directly, but the root page remains at its static `正在连接机器人...` state. This proves that the HTTP service and files are reachable, while leaving two indistinguishable failure modes: the page may load a stale script as a subresource, or JavaScript may fail before `bootstrap()` updates the login dialog.

The current HTML provides no signal between document load and application bootstrap, so either failure looks like an unavailable robot.

## Selected Approach

Keep the application in the external `app.js` module and add two independent safeguards:

1. During ESP32 bundle creation, calculate the first 12 hexadecimal characters of the SHA-256 digest of `app.js` and replace the single `__APP_ASSET_VERSION__` placeholder in its script URL. The source `web/index.html` remains readable, while the generated bundle contains the concrete digest. Changing the JavaScript therefore produces a new URL without requiring manual version maintenance.
2. Add a small inline startup probe after the login dialog exists and before loading `app.js`. It changes the initial status to `正在加载控制程序...`, records errors that happen before application bootstrap completes, and reports an external-script load failure through the existing login error area.

Once `app.js` begins normally, it marks the probe complete before running the existing bootstrap flow. Authentication, setup, API, WebSocket, resource, STM32, display, and motion behavior remain unchanged.

## Component Boundaries

- `tools/build_esp_bundle.py` owns deterministic asset-version substitution in generated output only.
- `web/index.html` owns the minimal pre-application status and script-load error presentation.
- `web/app.js` owns the transition from the startup probe to the existing API bootstrap process.

The probe exposes only a small completion function on `window`; it does not duplicate authentication or API logic.

## Error Handling

- If the external script cannot be loaded, the login dialog displays `控制程序加载失败` and suggests refreshing the page.
- If JavaScript fails before application startup completes, the dialog displays `控制程序启动失败` plus only the exception name and source line number. It does not display credentials, request headers, API keys, configuration values, or arbitrary exception text.
- API bootstrap failures continue through the existing `showAuth(false)` and `auth-error` path.
- Diagnostic handlers stop changing the login dialog after application startup is marked complete, preventing later UI errors from overwriting normal state.

## Verification

Host tests will verify that:

- the source HTML contains exactly one `__APP_ASSET_VERSION__` placeholder;
- a generated bundle replaces it with the current `app.js` digest;
- no unresolved placeholder remains in the generated bundle;
- the startup probe and application completion hook are both present;
- the existing web UI structure and full Python test suite still pass;
- `app.js` passes JavaScript syntax validation.

Hardware acceptance will preserve `/config`, send no motion command, and proceed as follows:

1. deploy the generated ESP32 bundle;
2. allow the deployment reset only after notifying the user;
3. open the root page on the phone without manually opening `app.js` first;
4. confirm the page advances to the enabled password form or displays a concrete diagnostic;
5. confirm the existing password succeeds and STM32 status remains available.
