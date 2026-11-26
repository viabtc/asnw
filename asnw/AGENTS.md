# asnw: notes for AI agents

asnw is an asynchronous network library in C on top of libev
(github.com/haipome/asnw). This directory is the whole library. The only
dependencies are libev and pthread. Link with `-lasnw -lev -lpthread`, or
compile the `.c` files of this directory into the program.

**The headers are the documentation.** Each `nw_*.h` starts with a comment
that explains the module, and every callback and function is described
where it is declared. Read the header of a module before using or changing
it; do not guess a signature.

## Modules

| Module | What it is | Reach for it when |
|---|---|---|
| `nw_svr` | Server: tcp, udp, unix stream / dgram / seqpacket, same callbacks for all | You write a server with a protocol of your own |
| `nw_clt` | Client: one connection to one server (address or name), reconnects by itself | A service keeps a long connection to another one |
| `nw_hub` | A `nw_svr` on N worker threads, one thread accepts | One thread is not enough |
| `nw_http` | HTTP/1.0 and 1.1 server. `thread_count` runs it on a hub | You expose an HTTP interface |
| `nw_ws` | WebSocket server (RFC 6455). `thread_count` runs it on a hub | You push to browsers or apps |
| `nw_job` | Thread pool: work on worker threads, result back on the loop thread | Something blocks: a database query, a disk read |
| `nw_req` | HTTP GET and POST on a thread of its own, response back on the loop thread | The occasional request inside a trusted network: fetching a configuration, posting an alert |
| `nw_dns` | Name lookup (getaddrinfo) off the loop thread | You resolve a name yourself; `nw_clt` already uses it |
| `nw_state` | Pending requests by id, each with a timeout | You wait for the answer to something you sent |
| `nw_timer` | One-shot and repeating timers | |
| `nw_ses` | A session: one socket and its buffers | You receive it in callbacks; `nw_ses_send` and flow control live here |
| `nw_evt` | The event loop of the thread: `nw_loop_run`, `nw_loop_break`, `nw_loop_release` | |
| `nw_sock` | Addresses: `nw_sock_cfg_parse("tcp@127.0.0.1:80")`, socket options | |
| `nw_buf` | Buffer pool and object cache | Internal; a program rarely needs it |
| `nw_dict` | Hash table with uint64 keys | Internal, usable on its own |

## The shape of a program

Fill a `cfg` struct and a `type` struct (the callbacks), create, start, run
the loop. **Always `memset` both structs to 0 first**: unset fields mean
"default" or "not used", and new fields are added over time.

```c
# include <string.h>
# include "nw_svr.h"

static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    /* where does the first message end? > 0: its size, 0: not complete
     * yet, < 0: broken data, close the connection */
    char *end = memchr(data, '\n', max);
    return end ? end - (char *)data + 1 : 0;
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ses_send(ses, data, size);
}

int main(void)
{
    nw_svr_bind bind;
    memset(&bind, 0, sizeof(bind));
    if (nw_sock_cfg_parse("tcp@127.0.0.1:1234", &bind.addr, &bind.sock_type) < 0)
        return 1;
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    cfg.max_pkg_size = 10240;
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_recv_pkg = on_recv_pkg;

    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    if (svr == NULL || nw_svr_start(svr) < 0)
        return 1;
    nw_loop_run();
    nw_svr_release(svr);
    nw_loop_release();
    return 0;
}
```

A client is the same with `nw_clt_cfg` (`nw_clt_cfg_parse` takes an
address or a name), `nw_clt_type`, `nw_clt_create`, `nw_clt_start`. An
HTTP server sets `on_request` in `nw_http_svr_type` and answers with
`nw_http_reply_body` or `nw_http_reply`. Complete examples are in the
`example/` directory of the repository: `echo_server.c`, `ping_clt.c`,
`http_server.c`, `ws_server.c`, `hub_server.c`, `req_get.c`.

## Rules that are easy to get wrong

1. **One loop per thread, no locks.** What a thread creates (server,
   client, timer, job pool) belongs to that thread and its callbacks run
   there, inside `nw_loop_run`. Never touch an object of another thread.
   The functions that may be called from any thread say so in their
   header: `nw_job_add`, the `nw_hub_*` send / close / post functions,
   `nw_http_reply_id`, the `nw_ws_*_id` and broadcast functions.

2. **Never block in a callback.** No sleep, no blocking read, no database
   call, no `getaddrinfo`. Blocking work goes to `nw_job`, a URL to
   `nw_req`, a name to `nw_dns`.

3. **Keep `ses->id`, not the `nw_ses` pointer.** A `nw_ses` is valid until
   its connection closes, then the memory is reused for another client.
   Across anything asynchronous keep the id (unique, never 0, never
   reused) and use `nw_svr_send(svr, id, ...)`, `nw_svr_get_clt`,
   `nw_http_reply_id`, `nw_ws_send_text_id`.

4. **What a callback is given is only valid inside the callback.** The
   `data` of `on_recv_pkg`, a `nw_http_request` and everything it points to
   (parsed in place), a `nw_req_response`, a `nw_job_entry`. Copy what you keep.

5. **`decode_pkg` only tells where a message ends.** It must not send,
   close, or keep state about the connection.

6. **After `nw_ses_send` returns -1, do not use the session**: for a
   connection it is closed already. 0 means sent or queued, not delivered.

7. **`nw_svr_close_clt` drops what was not sent yet.** To say something
   last, send it and call `nw_svr_shutdown_clt`.

8. **Sending is not limited by default.** `buf_limit` 0 queues without
   bound. A program that sends a lot to slow peers looks at
   `nw_ses_pending(ses)` and uses `on_drain`, or `nw_ses_pause_read`: the
   library reports the facts, the program decides the policy.

9. **A client may not be connected.** Check `nw_clt_connected(clt)` before
   sending. `on_close` is called once per lost connection or failed
   attempt; the client then reconnects every `reconnect_timeout` seconds
   by itself.

10. **Release order.** Release what adds work before what does it: a hub
    or server before the `nw_job` its callbacks add to. `nw_loop_release`
    comes last. A thread of your own that used the library calls it
    before it exits; on the main thread it is also what stops the threads
    of `nw_dns` and `nw_req`.

11. **Per connection data** comes from `on_privdata_alloc` /
    `on_privdata_free` in `nw_svr_type` and is found in `ses->privdata`;
    `ses->svr` is the `nw_svr`, and `svr->privdata` is what was given to
    `nw_svr_create`. For a client `ses->privdata` is what was given to
    `nw_clt_create`.

## Several threads

`nw_hub` (or `thread_count` in the cfg of `nw_http` / `nw_ws`) gives each
connection to one of N worker threads, where all its callbacks run. The
callbacks stay as they are if they only use per connection data; data
shared between connections is then used by several threads and is the
program's to lock, or to keep once per thread (set up in
`on_thread_start`). To reach a connection of another thread, use its id.

For blocking work on a hub, create one `nw_job` on the main thread, call
`nw_job_add` from the workers, and answer by id from `on_finish`, which
runs on the main thread.

## Decisions that are not bugs

These were considered and decided. Do not "fix" them, and do not propose
them again without a new reason:

- **Pools and caches never give memory back to the system.** Servers own
  their machine. The library also relies on it: memory returned to a
  `nw_cache` or `nw_buf_pool` stays readable (a session is still read after
  it is closed, an iterator steps off a deleted entry). Freeing it creates
  use-after-free.
- **`buf_limit` defaults to unlimited**, and there is no built-in rate
  limiting or backpressure policy. That belongs to the program.
- **`nw_clt` has no reconnect backoff, no idle timeout and no heartbeat.**
  Connections between services are few; heartbeats belong to the protocol
  above (see `wrapper/ut_rpc_clt.c` in the repository).
- **No TLS, no HTTP/2, no general HTTP client.** `nw_req` is GET and POST
  over plain HTTP inside a trusted network, on purpose: no https, no
  request headers, no retries. Put a proxy in front for TLS, use libcurl
  for a real client.
- **Timeouts of `nw_svr` are accurate to about a second**, checked by one
  timer that walks the connections.
- **`wrapper/ut_rpc`** in the repository is an example of a protocol on
  top of the library, not something to build production systems on.
- **Plain C, kept simple.** No C++, no new dependency, no configuration
  knob without a user for it. A fixed constant beats an option.

## Changing the library

Style, as in the existing files: 4 spaces, `# include` and `# define` with
a space after `#`, `snake_case` with the `nw_` prefix for everything
public, static helpers without it. Comments are short lower-case English
and say why, not what. A public function or callback is documented in the
header, where its users look.

The tests are in the repository, not in this directory. From its root:

```
make check            unit tests, debug build
make check-asan       AddressSanitizer + UBSan
make check-tsan       ThreadSanitizer
make check-valgrind   valgrind memcheck
make VARIANT=release check
make lint             gcc -Wextra and cppcheck
make check-asan T=t_echo      one test only
```

A change is not done until all of them pass, and their output has been
read: a sanitizer report is a failure even when the test itself passes.
Anything with threads is run several times under TSan. A fix comes with a
test in `unittest/t_*.c` that fails without it; a new module comes with
tests and a paragraph in `README.md`.

Things the code depends on that are easy to break:

- A callback may close its own connection, or release other ones, and the
  library goes on reading the session afterwards: see the note on pools
  above before touching anything that frees memory.
- A session is closed before its close callback runs, never after.
- One read per readable event for streams: the loop stops after a short
  read. Do not turn it back into "read until EAGAIN".
- Every socket is created non-blocking and close-on-exec, at creation
  (`SOCK_NONBLOCK | SOCK_CLOEXEC`, `accept4`), not with a later `fcntl`.
- Worker threads of the library block all signals; signals are for the
  threads of the program.
- Code that runs on a worker thread (`on_job` of `nw_job`, and the inside
  of `nw_dns` and `nw_req`) uses nothing of the library but what it was
  given.
