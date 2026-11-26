# ut_rpc

A small binary RPC protocol on top of asnw: a server (`rpc_svr`, a `nw_svr`) and a client (`rpc_clt`, a `nw_clt`) that exchange packets with a fixed header, an optional ext block, a body and a checksum.

It is an example of wrapping `nw_svr` and `nw_clt` into something higher level. It is not a part of the library and not meant for production: read it, then take it as the starting point of a protocol of your own. It leaves out what a production protocol needs, such as error reporting, a configurable packet size and versioning. What it adds to the core:

- **A packet format** with a magic number, size limits and a CRC-32, so a broken or foreign stream is detected instead of misread.
- **Heartbeats**, so both ends notice a dead connection within 3 seconds.
- **Failover**: the client takes a list of equivalent servers and moves to the next one when a connection fails or is lost.

The headers are the reference: `ut_rpc.h` (packet, `rpc_send`), `ut_rpc_svr.h`, `ut_rpc_clt.h`.

## Files

| File | What it is |
|------|------------|
| `ut_rpc.h`, `ut_rpc.c` | The packet, `rpc_decode` and `rpc_send`, the heartbeat constants |
| `ut_rpc_svr.h`, `ut_rpc_svr.c` | The server |
| `ut_rpc_clt.h`, `ut_rpc_clt.c` | The client, with failover |
| `ut_crc32.h`, `ut_crc32.c` | The checksum: standard CRC-32, 8 bytes at a time |
| `test_rpc_svr.c`, `test_rpc_clt.c` | Examples: an echo server and a client that pings it once a second |

## Build

From the top of the repository, `make` builds `build/debug/libutrpc.a` and the two examples in `build/debug/wrapper/`. Link with `-lutrpc -lasnw -lev -lpthread` and add `asnw/` and `wrapper/` to the include path.

The `makefile` in this directory also still builds the two examples on its own, once `asnw/libasnw.a` exists (`make -C ../asnw`).

Try it:

```
build/debug/wrapper/test_rpc_svr 'tcp@127.0.0.1:4000' &
build/debug/wrapper/test_rpc_svr 'tcp@127.0.0.1:4001' &
build/debug/wrapper/test_rpc_clt demo 'tcp@127.0.0.1:4000' 'tcp@127.0.0.1:4001'
```

Kill the first server and the client moves to the second.

## The packet

```
| header, 36 bytes | ext, ext_size bytes | body, body_size bytes |
```

All integers are little endian. The header:

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 4 | `magic` | `0x70656562`, filled in by `rpc_send` |
| 4 | 4 | `command` | What to do, defined by the application. 0 is the heartbeat |
| 8 | 2 | `pkg_type` | 0 request, 1 reply, 2 push |
| 10 | 4 | `result` | Result code of a reply, defined by the application |
| 14 | 4 | `crc32` | Checksum of the whole packet, filled in by `rpc_send` |
| 18 | 4 | `sequence` | Set by the requester and copied into the reply, to match the two |
| 22 | 8 | `req_id` | An id for the application, e.g. to trace a request across services |
| 30 | 4 | `body_size` | Size of the body |
| 34 | 2 | `ext_size` | Size of the ext block |

- A whole packet is at most 1M (`RPC_MAX_PKG_SIZE`), the ext block at most 65535 bytes.
- The content of ext and body is up to the application. ext is meant for what travels with a request but is not part of it, such as routing or tracing data.
- The checksum is the standard CRC-32 (IEEE 802.3, the one of zlib and zip, not CRC-32C), computed over the whole packet with the `crc32` field set to 0. A peer written in another language can use its ordinary crc32 function.
- `pkg_type` is a convention between the two ends, the library does not act on it: a request expects a reply with the same `sequence`, a push expects nothing back.

A packet with a bad magic, a size over the limit or a wrong checksum closes the connection.

## A server

```c
# include <string.h>
# include "ut_rpc_svr.h"

static void on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    /* pkg, pkg->ext and pkg->body are only valid during the call */
    rpc_pkg reply;
    memcpy(&reply, pkg, sizeof(reply));     /* same command, sequence, req_id */
    reply.pkg_type  = RPC_PKG_TYPE_REPLY;
    reply.result    = 0;
    reply.ext_size  = 0;
    reply.body      = "pong";
    reply.body_size = 4;
    rpc_send(ses, &reply);
}

int main(void)
{
    nw_svr_bind bind;
    if (nw_sock_addr_parse("tcp@127.0.0.1:4000", &bind.addr, &bind.sock_type) < 0)
        return 1;

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;

    rpc_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_recv_pkg = on_recv_pkg;

    rpc_svr *svr = rpc_svr_create(&cfg, &type);
    if (svr == NULL || rpc_svr_start(svr) < 0)
        return 1;
    nw_loop_run();
    return 0;
}
```

The config is a plain `nw_svr_cfg`, so a server can listen on several addresses of different types. `rpc_svr_create` overwrites two of its fields: `max_pkg_size` (1M) and `idle_timeout` (3 seconds, see Heartbeats). `on_open` and `on_close` are optional and behave as in `nw_svr`.

`svr->raw_svr` is the `nw_svr` underneath, for everything `rpc_svr` does not wrap: finding a connection by id, walking the connections, flow control. To push to a client, keep its `ses->id` and send when there is something to say:

```c
nw_ses *ses = nw_svr_get(svr->raw_svr, id);
if (ses)
    rpc_send(ses, &push);
```

## A client

```c
# include <string.h>
# include "ut_rpc_clt.h"

static void on_connect(nw_ses *ses, bool result)
{
    if (!result)
        return;                             /* the next address is tried */
    rpc_clt *clt = ses->privdata;
    rpc_pkg pkg;
    memset(&pkg, 0, sizeof(pkg));
    pkg.command   = 100;
    pkg.pkg_type  = RPC_PKG_TYPE_REQUEST;
    pkg.sequence  = 1;
    pkg.body      = "ping";
    pkg.body_size = 4;
    rpc_clt_send(clt, &pkg);
}

static void on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    /* the reply: pkg->sequence says to which request */
}

int main(void)
{
    nw_addr addr[2];
    rpc_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "demo";
    cfg.addr_count = 2;
    cfg.addrs = addr;
    if (nw_sock_addr_parse("tcp@127.0.0.1:4000", &addr[0], &cfg.sock_type) < 0 ||
        nw_sock_addr_parse("tcp@127.0.0.1:4001", &addr[1], &cfg.sock_type) < 0)
        return 1;

    rpc_clt_type type;
    memset(&type, 0, sizeof(type));
    type.on_connect = on_connect;
    type.on_recv_pkg = on_recv_pkg;

    rpc_clt *clt = rpc_clt_create(&cfg, &type);
    if (clt == NULL || rpc_clt_start(clt) < 0)
        return 1;
    nw_loop_run();
    return 0;
}
```

**Sending.** Set the `rpc_pkg` to 0, fill in `command`, `pkg_type`, `sequence` and whatever else you use, point `ext` and `body` at your data, and call `rpc_clt_send` (or `rpc_send` with a session). The data is copied, and the magic and the checksum are filled in. Sending returns -1 while there is no connection, so check `rpc_clt_connected` first, or send from `on_connect`.

**Failover.** All addresses must be of the same type. The client starts with the first one. When a connect fails or the connection is lost, it goes through the list without delay, then waits `reconnect_timeout` before going through it again. `on_connect` is called for every attempt that ends, with `ses->peer_addr` telling which address it was. Requests that were waiting for a reply when the connection went away are not sent again: that is for the application to decide.

**Matching replies to requests** is not done by `ut_rpc`; `nw_state` is made for it. Add an entry when sending and use its id as the sequence; when the reply arrives, look the entry up and delete it; if none arrives, `on_timeout` of the state table is called:

```c
nw_state_entry *entry = nw_state_add(state, 5.0, 0);    /* 5 seconds to answer */
/* put what you need to continue in entry->data */
pkg.sequence = entry->id;
rpc_clt_send(clt, &pkg);

/* in on_recv_pkg */
nw_state_entry *entry = nw_state_get(state, pkg->sequence);
if (entry) {
    /* continue with entry->data */
    nw_state_del(state, pkg->sequence);
}
```

## Heartbeats

On stream and seqpacket connections the client sends a heartbeat (command 0) every second and the server sends it back. Heartbeats are handled inside `ut_rpc` and never reach `on_recv_pkg`.

- The client closes a connection on which no heartbeat came back for 3 seconds, and connects again, starting with the first address.
- The server closes a connection that was silent for 3 seconds. Anything the client sends counts, not only heartbeats. It is the idle timeout of `nw_svr`, checked about once a second.

The two numbers are `RPC_HEARTBEAT_INTERVAL` and `RPC_HEARTBEAT_TIMEOUT` in `ut_rpc.h`. They are compile time constants and both ends must agree on them.

Datagram clients have no connection, so no heartbeat is sent and nothing tells whether the server is there.

## Things to know

- **Command 0 is reserved** for the heartbeat. Use other numbers for your own packets.
- **Received packets live in the read buffer.** `pkg`, `pkg->ext` and `pkg->body` are valid during `on_recv_pkg` only; copy what you keep. The header is already in host byte order.
- **One thread.** Like the rest of asnw, a server or a client belongs to the thread whose loop runs it, and `rpc_send` must be called there. To do slow work for a request, hand it to `nw_job` and reply from its finish callback, finding the connection again by id.
- **Errors are silent.** A packet that fails to decode closes the connection and nothing is logged: `on_error_msg` of the session underneath is swallowed. Change `on_error_msg` in `ut_rpc_svr.c` and `ut_rpc_clt.c` if you want to see them.
- **Linux only**, like the library (`<endian.h>`).
- `rpc_clt_release` must not be called from one of the client's callbacks.

## Tests

`unittest/t_rpc.c` (request and reply, heartbeats), `t_rpc_decode.c` (sizes and checksum), `t_rpc_failover.c` (the first address is dead) and `t_crc32.c`, all run by `make check`.
