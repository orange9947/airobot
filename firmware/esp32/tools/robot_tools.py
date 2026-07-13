"""Validated whitelist translating model tools to device commands."""

from protocol.generated import protocol_ids

TOOL_SCHEMAS = [
    {
        "type": "function",
        "name": "robot_move",
        "description": "Move the differential-drive desktop robot for a short bounded duration.",
        "parameters": {
            "type": "object",
            "properties": {
                "direction": {"type": "string", "enum": ["forward", "backward", "left", "right"]},
                "speed_percent": {"type": "integer", "minimum": 10, "maximum": 100},
                "duration_ms": {"type": "integer", "minimum": 100, "maximum": 2000},
            },
            "required": ["direction", "speed_percent", "duration_ms"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "type": "function",
        "name": "robot_stop",
        "description": "Immediately stop the robot and enter its stopped state.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
        "strict": True,
    },
    {
        "type": "function",
        "name": "robot_set_expression",
        "description": "Set the OLED face expression.",
        "parameters": {
            "type": "object",
            "properties": {"expression": {"type": "string", "enum": ["neutral", "happy", "sad", "thinking", "surprised", "sleepy"]}},
            "required": ["expression"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "type": "function",
        "name": "robot_set_mode",
        "description": "Return the robot to idle. The model cannot enter manual or AI mode itself.",
        "parameters": {
            "type": "object",
            "properties": {"mode": {"type": "string", "enum": ["idle"]}},
            "required": ["mode"],
            "additionalProperties": False,
        },
        "strict": True,
    },
]

EXPRESSIONS = {
    "neutral": protocol_ids.EXPRESSION_NEUTRAL,
    "happy": protocol_ids.EXPRESSION_HAPPY,
    "sad": protocol_ids.EXPRESSION_SAD,
    "thinking": protocol_ids.EXPRESSION_THINKING,
    "surprised": protocol_ids.EXPRESSION_SURPRISED,
    "sleepy": protocol_ids.EXPRESSION_SLEEPY,
}


class RobotTools:
    def __init__(self, device, config):
        self.device = device
        self.config = config

    async def execute(self, call):
        name = call.get("name")
        arguments = call.get("arguments") or {}
        if name == "robot_move":
            return await self._move(arguments)
        if name == "robot_stop":
            command_id = await self.device.stop()
            return {"ok": True, "command_id": command_id, "state": "estop"}
        if name == "robot_set_expression":
            expression = arguments.get("expression")
            if expression not in EXPRESSIONS:
                return {"ok": False, "error": "invalid_expression"}
            command_id = await self.device.set_expression(EXPRESSIONS[expression])
            return {"ok": True, "command_id": command_id, "expression": expression}
        if name == "robot_set_mode" and arguments.get("mode") == "idle":
            command_id = await self.device.set_mode(protocol_ids.MODE_IDLE)
            return {"ok": True, "command_id": command_id, "mode": "idle"}
        return {"ok": False, "error": "tool_not_allowed"}

    async def _move(self, arguments):
        direction = arguments.get("direction")
        speed = arguments.get("speed_percent")
        duration = arguments.get("duration_ms")
        if direction not in ("forward", "backward", "left", "right"):
            return {"ok": False, "error": "invalid_direction"}
        if not isinstance(speed, int) or not 10 <= speed <= 100:
            return {"ok": False, "error": "invalid_speed"}
        if not isinstance(duration, int) or not 100 <= duration <= 2000:
            return {"ok": False, "error": "invalid_duration"}
        motion = self.config.config["motion"]
        rate = max(1, int(motion["soft_rate_sps"]) * speed // 100)
        steps = max(1, rate * duration // 1000)
        wheel_steps = {
            "forward": (steps, steps),
            "backward": (-steps, -steps),
            "left": (-steps, steps),
            "right": (steps, -steps),
        }[direction]
        result = await self.device.move(
            wheel_steps[0], wheel_steps[1], rate,
            int(motion["accel_sps2"]), duration,
        )
        result.update(direction=direction, rate_sps=rate)
        return result
