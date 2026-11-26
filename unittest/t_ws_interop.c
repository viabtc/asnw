/*
 * nw_ws_svr against a real client: the Python "websockets" library. The
 * server runs in a child process, the client script in the parent.
 * Skipped when python3 or the library is not installed.
 */
# include "common.h"
# include "nw_ws.h"
# include <sys/wait.h>

static void on_open(nw_ses *ses)
{
    nw_ws_send_text(ses, "welcome");
}

static void on_message(nw_ses *ses, bool binary, void *data, size_t size)
{
    if (!binary && strcmp(data, "bye") == 0) {
        nw_ws_close(ses, NW_WS_CLOSE_NORMAL, "see you");
    } else if (!binary && strcmp(data, "everyone") == 0) {
        nw_ws_broadcast_text(nw_ws_svr_of(ses), "to all");
    } else if (binary) {
        nw_ws_send_binary(ses, data, size);
    } else {
        nw_ws_send_text(ses, data);
    }
}

static void run_server(int port)
{
    char tcp_cfg[64];
    snprintf(tcp_cfg, sizeof(tcp_cfg), "tcp@127.0.0.1:%d", port);
    nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse(tcp_cfg, &bind.addr, &bind.sock_type), 0);
    nw_ws_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.max_message_size = 1024 * 1024;
    cfg.protocol = "echo";
    nw_ws_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_open = on_open;
    type.on_message = on_message;
    nw_ws_svr *svr = nw_ws_svr_create(&cfg, &type, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_ws_svr_start(svr), 0);
    ut_run_for(60.0);
    _exit(0);
}

static const char *script =
"import sys\n"
"from websockets.sync.client import connect\n"
"from websockets.exceptions import ConnectionClosedOK, ConnectionClosed, InvalidStatus\n"
"url = sys.argv[1]\n"
"with connect(url, subprotocols=['other', 'echo'], compression=None, max_size=2**21) as ws:\n"
"    assert ws.subprotocol == 'echo', ws.subprotocol\n"
"    assert ws.recv() == 'welcome'\n"
"    for text in ['hello', '', 'x' * 125, 'y' * 126, 'z' * 70000, '\\u4f60\\u597d \\U0001f600']:\n"
"        ws.send(text)\n"
"        assert ws.recv() == text, len(text)\n"
"    for size in [0, 1, 4096, 4097, 65535, 65536, 1000000]:\n"
"        data = bytes(i * 7 % 256 for i in range(size))\n"
"        ws.send(data)\n"
"        assert ws.recv() == data, size\n"
"    # a message sent in fragments\n"
"    ws.send(['frag', 'men', 'ted'])\n"
"    assert ws.recv() == 'fragmented'\n"
"    ws.send([b'ab', b'', b'cd' * 5000])\n"
"    assert ws.recv() == b'ab' + b'cd' * 5000\n"
"    # ping with a payload, the pong has to carry it back\n"
"    assert ws.ping(b'payload').wait(5)\n"
"    # a second client: the broadcast reaches both\n"
"    with connect(url, compression=None) as other:\n"
"        assert other.recv() == 'welcome'\n"
"        ws.send('everyone')\n"
"        assert ws.recv() == 'to all'\n"
"        assert other.recv() == 'to all'\n"
"    # the server closes with a code and a reason\n"
"    ws.send('bye')\n"
"    try:\n"
"        ws.recv()\n"
"        raise AssertionError('expected a close')\n"
"    except ConnectionClosedOK as e:\n"
"        assert e.rcvd.code == 1000 and e.rcvd.reason == 'see you', e.rcvd\n"
"# the client closes: the server answers with the same code\n"
"with connect(url, compression=None) as ws:\n"
"    ws.recv()\n"
"    ws.close(1001, 'leaving')\n"
"    assert ws.close_code == 1001, ws.close_code\n"
"# a message over the limit gets the connection closed with 1009\n"
"with connect(url, compression=None) as ws:\n"
"    ws.recv()\n"
"    try:\n"
"        ws.send(b'x' * (1024 * 1024 + 1))\n"
"        ws.recv()\n"
"        raise AssertionError('expected a close')\n"
"    except ConnectionClosed as e:\n"
"        assert e.rcvd is not None and e.rcvd.code == 1009, e.rcvd\n"
"# compression is offered by default and has to be declined cleanly\n"
"with connect(url) as ws:\n"
"    assert ws.recv() == 'welcome'\n"
"    ws.send('plain')\n"
"    assert ws.recv() == 'plain'\n"
"print('ok')\n";

int main(void)
{
    if (system("python3 -c 'import websockets.sync.client' > /dev/null 2>&1") != 0) {
        fprintf(stderr, "skipped: python3 with the websockets library is not installed\n");
        return 0;
    }

    int port = ut_free_port(SOCK_STREAM);
    char file[128], cmd[512], out[4096];
    snprintf(file, sizeof(file), "%s/client.py", ut_tmpdir());
    FILE *fp = fopen(file, "w");
    CHECK(fp != NULL);
    fputs(script, fp);
    fclose(fp);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        run_server(port);
    usleep(200 * 1000);

    /* only stdout is checked, warnings of the library go to the test log */
    snprintf(cmd, sizeof(cmd), "python3 %s ws://127.0.0.1:%d/chat?x=1", file, port);
    fp = popen(cmd, "r");
    CHECK(fp != NULL);
    size_t len = fread(out, 1, sizeof(out) - 1, fp);
    out[len] = 0;
    int status = pclose(fp);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (status != 0 || strcmp(out, "ok\n") != 0) {
        fprintf(stderr, "client failed (%d):\n%s\n", status, out);
        return 1;
    }
    return 0;
}
