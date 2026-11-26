# asnw

A small asynchronous network library for Linux, written in C on top of [libev](http://software.schmorp.de/pkg/libev.html).

You describe a server or a client with a config struct and a handful of callbacks; asnw does the sockets, buffering, message framing, reconnecting and timers. One API covers five transports: TCP, UDP, and unix stream / datagram / seqpacket sockets (including file descriptor passing). An HTTP/1.1 server and a WebSocket server are included.

No dependency other than libev. One event loop per thread and no locks in the callbacks; `nw_hub` runs a server on several threads.

The headers in `asnw/` are the reference: every struct field, callback and function is documented there, and each header starts with an overview of its module.

## Modules

The core:

| Module | What it is |
|--------|------------|
| `nw_svr` | Server. One instance can listen on several addresses of different types at once. Finds connections by id, walks them, times them out and closes them in an orderly way. |
| `nw_clt` | Client. Keeps a connection to one server, given by address or by name, and reconnects automatically. |
| `nw_hub` | A `nw_svr` on several threads: one thread accepts, each connection lives on one of N worker threads. Same callbacks, no locks, and sending by id across threads. |
| `nw_timer` | Call a function after an interval, once or repeatedly. |
| `nw_state` | Table of pending states that expire, for request tracking with timeouts. |
| `nw_job` | Thread pool. Work runs on worker threads, results come back on the loop thread. |
| `nw_dns` | Name lookup without blocking the loop: the system resolver runs on worker threads, the answer comes back on the loop thread. Used by `nw_clt`. |
| `nw_req` | A small HTTP client: GET or POST from a thread of its own, the response comes back on the loop thread. For the occasional request inside a trusted network, such as fetching a configuration or posting an alert. No https. |
| `nw_dict` | Hash table from integer keys to pointers. Used by `nw_svr` and `nw_state`, usable on its own. |
| `nw_buf` | Buffer pool, buffer list and a fixed size object cache. |
| `nw_sock` | Address parsing and socket option helpers. |
| `nw_evt` | The event loop (`nw_loop_run`, `nw_loop_break`). |
| `nw_ses` | A session: one socket and its buffers. Used by `nw_svr` and `nw_clt`; you receive it in callbacks. |

Protocols built on the core, which knows nothing about them:

| Module | What it is |
|--------|------------|
| `nw_http` | HTTP/1.0 and 1.1 server, with zero copy request parsing. On one thread, or on several with `thread_count`. |
| `nw_ws` | WebSocket server (RFC 6455): messages, broadcast, pings. On one thread, or on several with `thread_count`. |

## Build

```
sudo apt-get install build-essential libev-dev
make            # library, wrapper, examples and tests into build/debug
make check      # run the tests
```

The library is `build/debug/libasnw.a` (`make VARIANT=release` for an optimized one in `build/release`). To use it, add `asnw/` to the include path and link with `-lasnw -lev -lpthread`.

`asnw/makefile` also still builds `asnw/libasnw.a` on its own.

## A server in 40 lines

A line based echo server listening on TCP and on a unix socket:

```c
# include <stdio.h>
# include <string.h>
# include "nw_svr.h"

/* where does a message end? return its size, 0 if incomplete, < 0 if broken */
static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *end = memchr(data, '\n', max);
    return end ? (int)(end - (char *)data) + 1 : 0;
}

/* called once per complete message */
static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ses_send(ses, data, size);
}

int main(void)
{
    nw_svr_bind bind[2];
    if (nw_sock_addr_parse("tcp@127.0.0.1:1234", &bind[0].addr, &bind[0].sock_type) < 0 ||
        nw_sock_addr_parse("stream@/tmp/echo.sock", &bind[1].addr, &bind[1].sock_type) < 0)
        return 1;

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 2;
    cfg.binds = bind;
    cfg.max_pkg_size = 10240;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_recv_pkg = on_recv_pkg;

    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    if (svr == NULL || nw_svr_start(svr) < 0)
        return 1;

    nw_loop_run();
    return 0;
}
```

`decode_pkg` and `on_recv_pkg` are required. The optional callbacks are `on_open`, `on_close`, `on_error_msg`, `on_accept`, `on_recv_fd` and `on_privdata_alloc` / `on_privdata_free` for per connection data. `nw_svr.h` documents each one.

A client is built the same way from `nw_clt_cfg` and `nw_clt_type`, then `nw_clt_start`. See `example/ping_clt.c`.

## An HTTP server

`nw_http` turns the same machinery into an HTTP server: you get a parsed request, you give back a response.

```c
# include <string.h>
# include "nw_http.h"

static void on_request(nw_ses *ses, nw_http_request *req)
{
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/") == 0) {
        nw_http_reply_body(ses, 200, "text/plain", "hello\n", 6);
        return;
    }
    if (strcmp(req->path, "/echo") == 0) {
        const char *type = nw_http_get_header(req, "Content-Type");
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        nw_http_add_header(&rsp, "Content-Type", type ? type : "application/octet-stream");
        rsp.body = req->body;
        rsp.body_size = req->body_size;
        nw_http_reply(ses, &rsp);
        return;
    }
    nw_http_reply_body(ses, 404, "text/plain", "not found\n", 10);
}

int main(void)
{
    nw_svr_bind bind;
    if (nw_sock_addr_parse("tcp@127.0.0.1:8080", &bind.addr, &bind.sock_type) < 0)
        return 1;

    nw_http_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;

    nw_http_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_request = on_request;

    nw_http_svr *svr = nw_http_svr_create(&cfg, &type, NULL);
    if (svr == NULL || nw_http_svr_start(svr) < 0)
        return 1;

    nw_loop_run();
    return 0;
}
```

- **Zero copy reads.** The request is parsed in place: method, path, query and every header name and value are C strings pointing into the connection's read buffer. Nothing is allocated or copied, which also means the request is only valid inside `on_request`.
- **Answering later.** Keep `ses->id` and call `nw_http_reply_id(svr, id, &rsp)` when the result is ready, for instance from a `nw_job` callback. If the client has left, nothing happens.
- **Several threads.** Set `thread_count` in the cfg and the server runs on a `nw_hub`: the thread that created it accepts, the connections live on that many worker threads and `on_request` runs there, for different connections at the same time. `nw_http_reply_id` works from any thread. To answer later without crossing threads, create what each worker needs (a `nw_job`, a client) in `on_thread_start`. Or share one `nw_job` created on the main thread: the workers call `nw_job_add`, and its `on_finish`, on the main thread, answers with `nw_http_reply_id`. Data shared between requests is yours to lock. 0, the default, means no threads at all.
- **Supported:** HTTP/1.0 and 1.1, keep-alive, `Content-Length` and chunked request bodies, `Expect: 100-continue`, `HEAD`, idle and slow-request timeouts.
- **Not supported:** TLS, HTTP/2, WebSocket, streamed responses, pipelining behind a request that is answered later. A request has to fit in `max_request_size` (64K by default).
- **Strict.** Requests that are malformed or ambiguous about where they end are answered with 400 and the connection is closed. See the list at the top of `nw_http.h`.

## A WebSocket server

`nw_ws` is a WebSocket server on a port of its own. Clients upgrade with the usual handshake; after that you get whole messages and send whole messages.

```c
# include <string.h>
# include "nw_ws.h"

/* look at the upgrade request, return < 0 to refuse the client */
static int on_upgrade(nw_ses *ses, nw_http_request *req)
{
    return strcmp(req->path, "/chat") == 0 ? 0 : -1;
}

static void on_open(nw_ses *ses)
{
    nw_ws_send_text(ses, "welcome");
}

/* data[size] is '\0', a text message is a C string */
static void on_message(nw_ses *ses, bool binary, void *data, size_t size)
{
    if (binary)
        nw_ws_close(ses, NW_WS_CLOSE_UNSUPPORTED, "text only");
    else
        nw_ws_broadcast_text(nw_ws_svr_of(ses), data);
}

int main(void)
{
    nw_svr_bind bind;
    if (nw_sock_addr_parse("tcp@127.0.0.1:8081", &bind.addr, &bind.sock_type) < 0)
        return 1;

    nw_ws_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.buf_limit = 16;         /* a client that falls this far behind is dropped */
    cfg.ping_interval = 20;

    nw_ws_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_upgrade = on_upgrade;
    type.on_open = on_open;
    type.on_message = on_message;

    nw_ws_svr *svr = nw_ws_svr_create(&cfg, &type, NULL);
    if (svr == NULL || nw_ws_svr_start(svr) < 0)
        return 1;

    nw_loop_run();
    return 0;
}
```

- **Zero copy reads.** A message that comes in one frame is unmasked in place and handed over where it lies in the read buffer; only a message split into several frames is put together in a buffer of its own.
- **Per connection data.** `on_privdata_alloc` / `on_privdata_free` give every connection a struct of yours, reached with `nw_ws_privdata(ses)`. The upgrade request is only valid in `on_upgrade`, so that is where to copy what you need from it.
- **Pushing.** `nw_ws_broadcast_text` sends to everybody. To push to one client from outside its callbacks, keep `ses->id` and use `nw_ws_send_text_id(svr, id, ...)`.
- **Several threads.** Set `thread_count` in the cfg and the server runs on a `nw_hub`: the thread that created it accepts, the connections live on that many worker threads and their callbacks run there. Sending and closing by id and the broadcasts work from any thread. The callbacks stay as they are if they only use per connection data; data shared between connections is yours to lock. 0, the default, means no threads at all.
- **Keeping connections alive.** With `ping_interval` set the server pings silent clients; one that stays silent for `idle_timeout` is closed.
- **Not supported:** TLS, compression (it is declined, clients fall back), UTF-8 validation of text messages, plain HTTP on the same port. A message has to fit in `max_message_size` (64K by default).

## Addresses

Addresses are written `protocol@address` and parsed with `nw_sock_addr_parse`:

| Example | Transport |
|---------|-----------|
| `tcp@127.0.0.1:3333` | TCP |
| `tcp@[::1]:3333` | TCP over IPv6 |
| `udp@127.0.0.1:3333` | UDP |
| `stream@/tmp/a.sock` | unix stream |
| `dgram@/tmp/a.sock` | unix datagram |
| `seqpacket@/tmp/a.sock` | unix seqpacket |

The protocol name is case insensitive. Hosts are numeric addresses: `nw_sock_addr_parse` does not resolve names. A client can be given a name in place of the ip, with `nw_clt_cfg_parse`: see "A server given by name" below.

## Things to know

**Messages.** On stream connections `decode_pkg` finds message boundaries, and a message can be at most `max_pkg_size` bytes. On datagram and seqpacket sockets every packet is one message; a packet for which `decode_pkg` returns 0 is dropped.

**Session lifetime.** A `nw_ses *` is valid until its connection closes, and the memory is reused for later connections. Across asynchronous work (a job, a state, a timer) keep `ses->id` instead of the pointer, and get the connection back with `nw_svr_get(svr, id)`, which returns NULL if the client is gone; `nw_svr_send(svr, id, ...)` does both steps. Ids are never 0 and never reused.

**Send buffering.** `nw_ses_send` writes directly when it can and queues the rest. `buf_limit` caps that queue in units of `max_pkg_size` buffers; the default 0 means unlimited, so set a limit on servers that face untrusted clients. Datagram sends are never queued: a full socket buffer is reported as an error.

**Flow control.** Dropping a peer that falls behind is the default and often the right thing. When it is not (a proxy, a relay, a big transfer), the library hands you the facts and the switches and leaves the policy to you: `nw_ses_pending(ses)` is the number of bytes still queued for a session, `on_drain` is called when that queue has been written out, and `nw_ses_pause_read` / `nw_ses_resume_read` stop and restart the input of a session, which makes the kernel hold its sender back. Passing data from `up` to `down` without piling it up is then two lines: pause `up` when `nw_ses_pending(down)` is over your mark, resume it in `on_drain` of `down`. All of it works on the connections of `nw_http` and `nw_ws` too.

**Timeouts.** `idle_timeout` in the server config (optional) drops connections that send nothing for that long. `on_timeout` lets you decide instead: left open, a silent connection times out again one period later, and `ses->active_time` tells how long it has been silent, which is all a "ping, then give up" policy needs. A single connection can be given its own timeout (`nw_svr_set_idle_timeout`) or a deadline that activity does not push back (`nw_svr_set_deadline`). Timeouts are accurate to about a second.

**Closing.** `nw_svr_close` closes at once and drops unsent data. To say something last and have it arrive, send it and call `nw_svr_shutdown`. Either way `on_close` is called exactly once, and by then the connection is already out of the server: it is not counted, not found by id, not visited by the iterator, and sending on it returns -1. So the callback is free to send to the other connections or close them; "tell everybody that this one left" needs no care.

**Walking the connections.** `nw_svr_iter_init` / `nw_svr_next` visit every connection of a server. Connections may be closed while walking, the current one or any other.

**Datagram servers.** The session passed to `on_recv_pkg` is the listening one, and `nw_ses_send` replies to the sender of the datagram being handled. Reply inside the callback, or copy `ses->peer_addr`.

**Memory.** A connection with unfinished input holds one buf of `max_pkg_size` bytes, and what waits to be sent is kept in such bufs. They come from the pool of the server, which reuses them. Once a minute a server gives the bufs it has no use for back to the system: it keeps as many free ones as it has in use, and at least 64. The memory of a peak is thus returned within a minute or two, without anything to set or to call; `nw_hub`, `nw_http` and `nw_ws` are built on `nw_svr` and do the same.

**Clients.** A stream client retries every `reconnect_timeout` seconds (default 1.0), which is also how long a connect attempt may take. `on_close` is called once per lost connection or failed attempt; return > 0 from it to retry at once, or call `nw_clt_stop` from it to stay closed. Check `nw_clt_connected` before sending. A datagram client is usable right after `nw_clt_start` and ignores UDP packets from any address other than its server.

**A server given by name.** `nw_clt_cfg_parse` takes what `nw_sock_addr_parse` takes and also a name, as in `tcp@db.internal:5432`. The name is looked up with the system resolver (`/etc/hosts`, DNS ...) on worker threads, so the loop never waits for it. When a name has several addresses they are tried one after the other and count as one attempt: `on_connect(false)` and `on_close` come once, after the last one has failed. The name is looked up again after every lost connection and every failed round, so a client follows a name that moves. A lookup that fails is reported through `on_error_msg` and counts as a failed attempt. A datagram client by name cannot send until its lookup is done: `on_connect(true)` tells when. It has no connection to lose, so its name is looked up once, when it starts: it keeps sending to the first address it got until `nw_clt_start` is called again. A server picked in `on_close` by changing `ses->peer_addr` is for clients given by address only: by name, the address always comes from the lookup. To resolve a name yourself, see `nw_dns.h`.

**Fetching a url.** `nw_req_get("http://config.internal:8500/v1/app", 3.0, on_response, privdata)` makes the request on a thread of its own and calls `on_response(error, rsp, privdata)` on the loop thread, with the status, the headers and the body. A status other than 200 is not an error: `error` is for requests that got no response (name not found, connection refused, timeout, a broken response), and `rsp->error_msg` says which. The timeout is for the whole request. `nw_req_post(url, content_type, body, size, timeout, on_response, privdata)` sends a body the same way, as `application/json` when `content_type` is NULL; a request that failed may still have been received, and is never sent twice. The callback may be NULL when the outcome does not matter. What is put into a url (the value of a parameter) is encoded first with `nw_req_url_encode`. It is plain HTTP for a trusted network: no https, no redirects, no request headers of your own, a new connection for each request, and at most `NW_REQ_THREADS` (2) requests at a time per loop thread, which is what fetching a configuration or posting an alert now and then needs. See `nw_req.h`.

**Several threads.** `nw_hub` takes the same config and callbacks as `nw_svr` (in `cfg.svr` and `type.svr`) plus a thread count. A connection is given to one worker thread and stays there; its callbacks run on that thread, where `ses->svr` is an ordinary `nw_svr` holding the connections of that thread, so everything above applies unchanged. Connections on different threads reach each other by id: `nw_hub_send`, `nw_hub_close`, `nw_hub_shutdown` and `nw_hub_broadcast` may be called from any thread and queue the request for the thread that owns the connection (`queue_limit` bounds what may wait for a thread that is stuck); `nw_hub_run` runs a function of yours on a given worker. What the library cannot do for you: data of your own that several connections share is now used by several threads, so lock it or keep one copy per thread, set up in `on_thread_start`. Stream and seqpacket binds only.

**Threads.** Every thread has an event loop of its own: what a thread creates (servers, clients, timers ...) runs on that thread, inside its `nw_loop_run`. A program with one thread need not think about any of this. Several threads can each run their own loop side by side, as nothing is shared between them, but there is no locking: an object and its connections may only be used by the thread that created them. To pass something to another thread, use a queue of your own and an `ev_async` watcher on the receiver's `nw_default_loop`. A thread other than the main one calls `nw_loop_release` before it exits. For blocking work there is `nw_job`: the work runs on worker threads and its `on_finish` callback runs back on the loop thread that created the pool. `nw_job_add` may be called from any thread, so several loop threads can share one pool: its results arrive on the thread that created it, which answers by id. Name lookups and `nw_req` use pools of their own: the first lookup or request of a loop thread starts two threads for it, which `nw_loop_release` stops.

**Unix sockets.** Binding replaces a stale socket file at the path and makes the socket accessible to all local users (mode 0777).

## Examples

Built into `build/debug/example/`, `build/debug/wrapper/` and `build/debug/c1000k/`.

| Program | What it shows |
|---------|---------------|
| `example/echo_server` | Echo server on any number of addresses: `echo_server 'tcp@127.0.0.1:1234' 'stream@/tmp/echo.sock'` |
| `example/http_server` | HTTP server, try it with curl: `http_server 'tcp@127.0.0.1:8080'`. `http_server 4 'tcp@...'` runs it on 4 threads |
| `example/hub_server` | Chat server on N threads: `hub_server 4 'tcp@127.0.0.1:1234'`, then `nc 127.0.0.1 1234` in a few terminals |
| `example/ws_server` | WebSocket chat room: `ws_server 'tcp@127.0.0.1:8081'`, then connect from a browser console. `ws_server 4 'tcp@...'` runs it on 4 threads |
| `example/ping_clt` | Client that pings a server once a second: `ping_clt 'tcp@127.0.0.1:1234'` |
| `example/req_get` | GET a url with `nw_req` and print the response: `req_get 'http://127.0.0.1:8080/hello'` |
| `example/listener_worker` | One process accepts and hands each connection to one of N worker processes by passing its fd: `listener_worker 'tcp@127.0.0.1:1234' 10` |
| `example/timer`, `example/state` | Timer and state table |
| `example/*_clt` | Plain socket clients, one per transport, for poking at a server |
| `c1000k/` | Server and client for a one million connection test |
| `wrapper/` | `ut_rpc`, an example of a higher level wrapper built on `nw_svr` and `nw_clt`. See [wrapper/README.md](wrapper/README.md) |
| `asnw/AGENTS.md` | Notes for AI coding agents that use or change the library: the modules, the rules that are easy to get wrong, and the design decisions that are not bugs |

## Testing

```
make check            # unit tests
make check-asan       # under AddressSanitizer and UBSan
make check-tsan       # under ThreadSanitizer
make check-valgrind   # under valgrind
make coverage         # line coverage per source file
make lint             # gcc -Wextra and cppcheck
```

The tests live in `unittest/`, one executable per test.

## License

MIT, see [LICENSE](LICENSE).
