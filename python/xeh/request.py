"""Request data copied out of libxeh's borrowed callback buffer."""

import ctypes
from dataclasses import dataclass

from ._native import NativeRequest


@dataclass(frozen=True)
class Request:
    sequence: int
    extension_id: int
    number: int
    client_id: int
    target_object: int
    payload: bytes

    @classmethod
    def from_native(cls, pointer):
        value = pointer.contents
        payload = ctypes.string_at(value.payload, value.payload_length) if value.payload_length else b""
        return cls(value.sequence, value.extension_id, value.request_number,
                   value.client_id, value.target_object, payload)

    def string(self):
        """Decode a UTF-8 payload, allowing at most three trailing pad bytes."""
        stripped = self.payload.rstrip(b"\0")
        if len(self.payload) - len(stripped) > 3:
            raise ValueError("too much string padding")
        return stripped.decode("utf-8")

    def _native(self):
        return NativeRequest(self.sequence, self.extension_id, self.number,
                             self.client_id, self.target_object, None, 0)
