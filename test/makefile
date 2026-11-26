all:
	gcc echo_server.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu11 -g -o echo_server
	gcc test_clt.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu11 -g -o test_clt
	gcc listener_worker.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu11 -g -o listener_worker
	gcc http_server.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu11 -g -o http_server
	gcc ws_server.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu11 -g -o ws_server
	gcc hub_server.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu11 -g -o hub_server

clean:
	rm -rf echo_server test_clt listener_worker http_server ws_server hub_server
