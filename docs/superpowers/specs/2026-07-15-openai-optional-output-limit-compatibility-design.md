# OpenAI Optional Output Limit Compatibility Design

## Problem

The OpenAI Responses adapter includes `max_output_tokens` in both initial and tool-result requests. A local, one-time hardware diagnostic reproduced the failing first request and received HTTP 400 with `Unsupported parameter: max_output_tokens` from the configured Responses-compatible relay. The diagnostic script was deleted and no relay address, model name, API key, or credential is recorded in this repository.

The official Responses API defines `max_output_tokens` as optional. Requiring every compatible relay to implement this optional request field prevents otherwise valid text and tool requests from reaching the model.

## Selected Approach

The OpenAI adapter will omit `max_output_tokens` from every Responses request. This applies equally to the initial request and all stateless tool-result continuation requests.

`OpenAIProvider` will use one private request-body builder so both paths contain the same fields:

- `model`;
- `input`;
- `tools`.

The existing configured output-token value remains in the shared configuration schema because the DeepSeek adapter still uses it as `max_tokens`. It is intentionally ignored by the OpenAI adapter. No provider-specific compatibility switch, host allowlist, relay address, or automatic retry is added.

## Bounds and Safety

Omitting the provider-side output-token hint does not remove local bounds:

- `AsyncJsonClient` rejects response bodies larger than 64 KiB;
- conversation history remains limited to the most recent 20 entries;
- tool execution remains limited to three rounds per user message;
- tool parameters and STM32 commands retain their existing whitelist and hardware validation;
- model output still cannot directly execute MicroPython or construct SPI frames.

If a response exceeds the local body limit, the existing `response body too large` error is returned and no automatic retry occurs. DeepSeek request behavior is unchanged.

## Verification

Host tests will assert that:

- the first OpenAI Responses request omits `max_output_tokens` even when the configuration contains a value;
- every OpenAI tool-result request also omits the field;
- model, input, tools, stateless continuation items, reasoning items, and function-call outputs remain unchanged;
- DeepSeek requests continue to include their configured `max_tokens` value;
- the complete Python suite and ESP32 bundle build pass.

Hardware acceptance preserves `/config` and keeps STM32 in ESTOP during deployment. After the notified ESP32 reset:

1. a plain OpenAI chat message must receive a normal reply instead of HTTP 400;
2. the user explicitly clears ESTOP only after confirming the page and STM32 link are healthy;
3. an expression-only prompt may call `robot_set_expression` and change the OLED;
4. no motion command, mode change, or automatic ESTOP clear is sent during acceptance.

Reference: [OpenAI Responses create API](https://developers.openai.com/api/reference/resources/responses/methods/create).
