# Network
A high performance asynchronous network programming library base on libev and in c

network includes the following modules:

- `nw_buf`   : buf manager
- `nw_evt`   : main loop
- `nw_sock`  : socket releated
- `nw_ses`   : network session manager
- `nw_timer` : timer, call a function after specify time, repeat or not repeat
- `nw_svr`   : server implement, one server can bind multi address in different sock type
- `nw_clt`   : client implement, auto reconnect
- `ne_state` : state machine with timeout

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
