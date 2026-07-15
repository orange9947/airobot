"""OpenAI Responses API adapter."""

try:
    import json
except ImportError:
    import ujson as json


class OpenAIProvider:
    MAX_SSE_OUTPUT_ITEMS = 16

    def __init__(self, client, config, api_key):
        self.client = client
        self.config = config
        self.api_key = api_key

    def _headers(self):
        return {"Authorization": "Bearer " + self.api_key}

    def _url(self):
        return self.config["base_url"].rstrip("/") + "/v1/responses"

    def _body(self, request_input, tools):
        return {
            "model": self.config["model"],
            "input": request_input,
            "tools": tools,
        }

    @classmethod
    def _decode_response(cls, text, response_headers):
        content_type = response_headers.get("content-type", "")
        media_type = content_type.split(";", 1)[0].strip().lower()
        if media_type == "application/json" or media_type.endswith("+json"):
            return json.loads(text) if text else {}
        if media_type != "text/event-stream":
            raise ValueError("unsupported response content type")

        completed = None
        done_items = {}
        normalized = text.replace("\r\n", "\n").replace("\r", "\n")
        for block in normalized.split("\n\n"):
            data_lines = []
            for line in block.split("\n"):
                field, separator, value = line.partition(":")
                if field != "data":
                    continue
                if separator and value.startswith(" "):
                    value = value[1:]
                data_lines.append(value)
            if not data_lines:
                continue
            data = "\n".join(data_lines)
            if not data or data.strip() == "[DONE]":
                continue
            event = json.loads(data)
            if not isinstance(event, dict):
                raise ValueError("invalid SSE event")
            event_type = event.get("type")
            if event_type == "response.failed":
                raise ValueError("response failed")
            if event_type == "response.output_item.done":
                output_index = event.get("output_index")
                if (
                    isinstance(output_index, bool)
                    or not isinstance(output_index, int)
                    or output_index < 0
                    or output_index >= cls.MAX_SSE_OUTPUT_ITEMS
                    or output_index in done_items
                ):
                    raise ValueError("invalid SSE output index")
                item = event.get("item")
                if not isinstance(item, dict):
                    raise ValueError("invalid SSE output item")
                done_items[output_index] = item
            elif event_type == "response.completed":
                if completed is not None:
                    raise ValueError("duplicate response completion")
                completed = event.get("response")
                if not isinstance(completed, dict):
                    raise ValueError("invalid completed response")
                if not isinstance(completed.get("output"), list):
                    raise ValueError("invalid completed output")

        if completed is None:
            raise ValueError("missing response completion")
        if not completed["output"] and done_items:
            indices = sorted(done_items)
            if indices != list(range(len(indices))):
                raise ValueError("incomplete SSE output")
            completed["output"] = [done_items[index] for index in indices]
        return completed

    def _parse(self, response, request_input):
        text_parts = []
        tool_calls = []
        if response.get("output_text"):
            text_parts.append(response["output_text"])
        for item in response.get("output", []):
            if item.get("type") == "function_call":
                try:
                    arguments = json.loads(item.get("arguments", "{}"))
                except ValueError:
                    arguments = {}
                tool_calls.append({"id": item.get("call_id", ""), "name": item.get("name", ""), "arguments": arguments})
            elif item.get("type") == "message":
                for content in item.get("content", []):
                    if content.get("type") == "output_text":
                        text_parts.append(content.get("text", ""))
        output_items = response.get("output", []) or []
        return {
            "text": "\n".join(part for part in text_parts if part),
            "tool_calls": tool_calls,
            "continuation": {"input": list(request_input) + list(output_items)},
        }

    async def create_turn(self, messages, tools):
        body = self._body(messages, tools)
        response = await self.client.post_json(
            self._url(),
            body,
            self._headers(),
            self.config.get("timeout_s", 60),
            response_parser=self._decode_response,
        )
        return self._parse(response, messages)

    async def submit_tool_results(self, continuation, results, _messages, tools):
        tool_outputs = [
            {"type": "function_call_output", "call_id": result["call_id"], "output": json.dumps(result["output"])}
            for result in results
        ]
        request_input = list(continuation["input"]) + tool_outputs
        body = self._body(request_input, tools)
        response = await self.client.post_json(
            self._url(),
            body,
            self._headers(),
            self.config.get("timeout_s", 60),
            response_parser=self._decode_response,
        )
        return self._parse(response, request_input)
