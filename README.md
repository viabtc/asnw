# ASNW: asynchronous network
A high performance asynchronous network programming library based on libev and in C.

Network including the following modules:

- `nw_buf`   : buf manager
- `nw_evt`   : main loop
- `nw_sock`  : socket releated
- `nw_ses`   : network session manager
- `nw_timer` : timer, call a function after specify time, repeat or not repeat
- `nw_svr`   : server implement, one server can bind multi address in different sock type
- `nw_clt`   : client implement, auto reconnect
- `nw_state` : state machine with timeout
- `nw_job`   : thread pool

### example
- `echo_server` : a single process server

see test/echo_server.c

```
./echo_server 'tcp@127.0.0.1:1234' 'stream@/tmp/echo_stream.sock'
```

- `listener_worker`: one process listen, multi process process connection

see test/listener_worker.c

```
./listener_worker 'tcp@127.0.0.1:1234' 10
```

### wrapper
Network is a very basic network programming library, but you can wrapper the basic nw_clt
and nw_svr to be more complex.

wrapper/ut_rpc is a wrapper example.
