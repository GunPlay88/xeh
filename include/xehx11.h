#ifndef XEH_X11_H
#define XEH_X11_H

/* X11 extension request minor opcodes. All multibyte fields use the
 * connecting X11 client's byte order, unlike the big-endian IPC wire. */
#define XEH_X11_QUERY_VERSION 0
#define XEH_X11_FORWARD 1
#define XEH_X11_LOOKUP 2

/* QUERY_VERSION is the four-byte xReq. Its 32-byte reply has protocol
 * major/minor in data00/data01. */

/* FORWARD starts with xReq, followed by extension_id:u32,
 * request_number:u16, reserved:u16, target_handle:u32, then opaque payload.
 * The 32-byte reply uses data00 for xeh_protocol_error and data01 for error
 * detail; its length counts four-byte units of padded opaque reply payload. */
#define XEH_X11_FORWARD_FIXED_SIZE 16U

/* LOOKUP starts with xReq, name_length:u16, reserved:u16 and the ASCII
 * extension name padded to four bytes. The reply carries id in data00,
 * major/minor versions in data01/data02, and request_count in data03.
 * An unknown name produces id 0. */
#define XEH_X11_LOOKUP_FIXED_SIZE 8U

#endif
