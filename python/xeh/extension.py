"""Declarative extension registration and request handler lookup."""


def request(number):
    if not isinstance(number, int) or not 1 <= number <= 65535:
        raise ValueError("request number must be in 1..65535")

    def decorate(method):
        method._xeh_request_number = number
        return method

    return decorate


class Extension:
    name = None
    version = (1, 0)
    event_count = 0
    error_count = 0
    requested_capabilities = 0

    def _handlers(self):
        handlers = {}
        for name in dir(type(self)):
            method = getattr(self, name)
            number = getattr(method, "_xeh_request_number", None)
            if number is not None:
                if number in handlers:
                    raise ValueError(f"duplicate request number {number}")
                handlers[number] = method
        return handlers
