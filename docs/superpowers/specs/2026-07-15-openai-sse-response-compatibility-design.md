# OpenAI JSON and SSE Response Compatibility Design

## Problem

The OpenAI Responses adapter expects every successful HTTP response body to be one JSON document. A local, one-time diagnostic showed that the configured Responses-compatible relay returns HTTP 200 with `Content-Type: text/event-stream` even when the request explicitly sets `stream` to `false`.

The stream contains completed `response.output_item.done` events followed by `response.completed`. The terminal response object may contain an empty `output` array even though the completed output-item events contain a full function call. Treating the entire stream as JSON produces `invalid JSON response` before any tool can be executed.

The diagnostic scripts were deleted. No relay address, model name, API key, response text, or credential is recorded in this repository.

## Selected Approach

Keep transport framing and provider semantics separate:

- `AsyncJsonClient` continues to own URL parsing, TLS, bounded body reads, HTTP status handling, and timeouts;
- `AsyncJsonClient.post_json()` accepts an optional response parser callback;
- after a successful bounded HTTP response is decoded as UTF-8, the client passes the response text and normalized response headers to that callback;
- when no callback is supplied, the existing single-JSON-document behavior remains unchanged;
- `OpenAIProvider` supplies a parser that accepts either a standard JSON response or a Responses SSE stream;
- `DeepSeekProvider` does not supply a parser and retains its current behavior.

Parser failures are normalized to `HttpClientError` without exposing Authorization headers or request bodies.

## OpenAI SSE Reconstruction

The OpenAI parser uses the response media type to select JSON or SSE. It accepts SSE only for `text/event-stream`; other non-JSON media types are rejected.

For SSE:

1. parse complete SSE event blocks and combine multiple `data:` lines in the same block;
2. ignore comments, blank data, `[DONE]`, and non-terminal delta events;
3. accept an output item only from `response.output_item.done`;
4. require `output_index` to be an integer from 0 through 15 and reject duplicate indices;
5. require every completed item to be an object;
6. reject `response.failed` and any malformed JSON event;
7. require exactly one valid `response.completed` response object;
8. require the terminal `output` value to be an array;
9. use the terminal response's non-empty `output` array when present; otherwise require completed indices to be contiguous from zero and reconstruct `output` in ascending index order.

The reconstructed response is then passed through the existing OpenAI text and function-call normalization. Reasoning items, message items, call IDs, tool names, and arguments retain their existing representation.

## Execution Safety

No streamed delta can trigger a hardware action. `LlmService` receives tool calls only after the complete response body has been read, the SSE terminal event has been validated, and the normalized Provider turn has been returned.

Existing bounds remain in force:

- response body limit: 64 KiB;
- completed output items: at most 16;
- conversation history: 20 entries;
- tool loop: at most three rounds;
- tool schema, argument validation, STM32 mode checks, ESTOP, and hardware limits are unchanged.

Missing terminal events, duplicate or invalid indices, malformed event JSON, failed responses, oversized bodies, and parser exceptions produce an error and execute no tool. There is no automatic provider retry.

## Verification

Host tests will cover:

- existing standard OpenAI JSON text and tool responses;
- an SSE function call whose terminal response has an empty output array;
- an SSE message response after a tool-result request;
- completed items ordered by `output_index`, not arrival order;
- multi-line `data:` blocks and `[DONE]` handling;
- malformed JSON, missing completion, `response.failed`, duplicate or non-contiguous indices, negative indices, index 16, non-object items, invalid terminal output, and duplicate completion;
- parser callback errors becoming bounded `HttpClientError` values;
- DeepSeek requests retaining normal JSON parsing;
- the complete Python suite and ESP32 bundle build.

Hardware acceptance preserves `/config` and keeps STM32 in ESTOP during deployment. A plain chat message must complete through the SSE path. Only after the user confirms the link is healthy and explicitly clears ESTOP may an expression-only prompt execute `robot_set_expression`; no movement, mode change, or automatic ESTOP clear is sent.

Reference: [OpenAI Responses create and streaming events](https://developers.openai.com/api/reference/resources/responses/methods/create).
