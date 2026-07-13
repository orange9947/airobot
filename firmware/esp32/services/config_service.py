"""Atomic public configuration and write-only secret storage."""

import os

try:
    import json
except ImportError:
    import ujson as json

from firmware.esp32.core.security import new_password_record, verify_password

DEFAULT_CONFIG = {
    "schema": 1,
    "wifi": {"ssid": "", "password": ""},
    "active_provider": "deepseek",
    "providers": {
        "openai": {"base_url": "https://api.openai.com", "model": "", "timeout_s": 60, "max_output_tokens": 256},
        "deepseek": {"base_url": "https://api.deepseek.com", "model": "deepseek-chat", "timeout_s": 60, "max_output_tokens": 256},
    },
    "motion": {"soft_rate_sps": 400, "accel_sps2": 600, "hold_ms": 200},
}


def _deep_copy(value):
    return json.loads(json.dumps(value))


class ConfigService:
    def __init__(self, root="/config"):
        self.root = root.rstrip("/") or "/"
        self.config_path = self.root + "/device.json"
        self.secrets_path = self.root + "/secrets.json"
        self.config = _deep_copy(DEFAULT_CONFIG)
        self.secrets = {"providers": {}, "admin_password": None}

    def _ensure_root(self):
        try:
            os.mkdir(self.root)
        except OSError:
            pass

    def _read_json(self, path, fallback):
        try:
            with open(path, "r") as handle:
                return json.load(handle)
        except (OSError, ValueError):
            return _deep_copy(fallback)

    def _atomic_write(self, path, value):
        self._ensure_root()
        temporary = path + ".tmp"
        backup = path + ".bak"
        with open(temporary, "w") as handle:
            json.dump(value, handle)
        try:
            os.remove(backup)
        except OSError:
            pass
        try:
            os.rename(path, backup)
        except OSError:
            pass
        os.rename(temporary, path)

    def load(self):
        self.config = self._read_json(self.config_path, DEFAULT_CONFIG)
        self.secrets = self._read_json(
            self.secrets_path, {"providers": {}, "admin_password": None}
        )
        self._validate_config(self.config)
        return self.config

    def _validate_config(self, config):
        if config.get("schema") != 1:
            raise ValueError("unsupported configuration schema")
        if config.get("active_provider") not in ("openai", "deepseek"):
            raise ValueError("invalid provider")
        motion = config["motion"]
        if not 1 <= int(motion["soft_rate_sps"]) <= 800:
            raise ValueError("invalid motion rate")
        if not 1 <= int(motion["accel_sps2"]) <= 1200:
            raise ValueError("invalid motion acceleration")
        if not 0 <= int(motion["hold_ms"]) <= 2000:
            raise ValueError("invalid motor hold time")
        for name in ("openai", "deepseek"):
            provider = config["providers"][name]
            base_url = provider.get("base_url", "")
            if not isinstance(base_url, str) or not (
                base_url.startswith("https://") or base_url.startswith("http://")
            ):
                raise ValueError("invalid provider base URL")
            if not isinstance(provider.get("model", ""), str):
                raise ValueError("invalid provider model")

    def update_public(self, patch):
        updated = _deep_copy(self.config)
        for key in ("active_provider", "providers", "motion", "wifi"):
            if key in patch:
                updated[key] = patch[key]
        self._validate_config(updated)
        self._atomic_write(self.config_path, updated)
        self.config = updated

    def set_provider_key(self, provider, api_key):
        if provider not in ("openai", "deepseek"):
            raise ValueError("invalid provider")
        providers = self.secrets.setdefault("providers", {})
        if api_key:
            providers[provider] = api_key
        else:
            providers.pop(provider, None)
        self._atomic_write(self.secrets_path, self.secrets)

    def provider_key(self, provider):
        return self.secrets.get("providers", {}).get(provider, "")

    def public_view(self):
        view = _deep_copy(self.config)
        wifi = view.get("wifi", {})
        view["wifi"] = {
            "ssid": wifi.get("ssid", ""),
            "password_configured": bool(wifi.get("password")),
        }
        view["keys_configured"] = {
            provider: bool(self.provider_key(provider)) for provider in ("openai", "deepseek")
        }
        return view

    def has_admin_password(self):
        return bool(self.secrets.get("admin_password"))

    def set_admin_password(self, password, iterations=20000):
        if len(password) < 8:
            raise ValueError("password must contain at least 8 characters")
        self.secrets["admin_password"] = new_password_record(password, iterations)
        self._atomic_write(self.secrets_path, self.secrets)

    def verify_admin_password(self, password):
        record = self.secrets.get("admin_password")
        return bool(record) and verify_password(password, record)
