all:
	gcc echo_server.c -I ../network/ -L ../network/ -lnetwork -lev -std=gnu11 -g -o echo_server
	gcc test_clt.c -I ../network/ -L ../network/ -lnetwork -lev -std=gnu11 -g -o test_clt
	gcc listener_worker.c -I ../network/ -L ../network/ -lnetwork -lev -std=gnu11 -g -o listener_worker

clean:
	rm -rf echo_server test_clt listener_worker
