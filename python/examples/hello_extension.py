"""Run with XEH_SOCKET_PATH set, or pass a socket path as argv[1]."""

import sys

import xeh


class HelloExtension(xeh.Extension):
    name = "XEH-PY-HELLO"
    version = (1, 0)
    event_count = 1

    @xeh.request(1)
    def hello(self, request):
        name = request.string()
        if not name or len(name) > 96:
            raise ValueError("name must contain 1..96 characters")
        response = {"message": f"Hello, {name}!"}
        self.connection.event(1, request.client_id, response,
                              request.target_object)
        return response


if __name__ == "__main__":
    xeh.run(HelloExtension(), sys.argv[1] if len(sys.argv) > 1 else None)
