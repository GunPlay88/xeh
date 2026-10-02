# Graphics buffer copy

This is the first graphics-facing XEH path, not a compositor or zero-copy
presentation API. The extension imports a sealed memfd with `xeh_import_shm()`
and requests `XEH_CAP_SHM`. The Xorg process must opt in with
`XEH_ENABLE_SHM=1`. After import succeeds, the extension calls
`xeh_bind_shm_client()` with the `client_id` from a forwarded request. Only
after the bind callback succeeds should it send the buffer handle to that
X11 client in its opaque reply.

The client sends XEH minor opcode `XEH_X11_BLIT_BUFFER` with the 32-byte
request layout in `include/xehx11.h`. All fields use the X11 client's byte
order. It supplies its own drawable and GC XIDs; Xorg checks `DixWriteAccess`
and `DixUseAccess` in that client's context. The host also verifies that the
handle is a live buffer owned by the named extension and bound to this client.
The GC controls clipping and raster operation. This request has no reply and
uses standard X11 errors.

Only `XEH_BUFFER_FORMAT_XRGB8888` is accepted for blitting. Pixels are
little-endian B, G, R, X bytes; the destination must be depth 24, 32 bits per
pixel, little-endian, and use the standard RGB masks. A window must use the
root visual. ARGB8888 and RGB565 can be imported, but cannot be blitted yet.
The source rectangle must fit inside the imported metadata, and the padded
copy is capped at 4 MiB. The memfd remains sealed and mapped read-only;
XEH copies just the requested rows to a short-lived staging buffer before
calling Xorg's `PutImage`. No framebuffer bytes go through IPC messages.

The binding is one-client-only and cannot be transferred. Releasing the
buffer, disconnecting its extension, or disconnecting its bound client
invalidates the handle. This API has standalone tests and an Xorg module
compile check, but has not been run against a live Xorg server in this VM.
