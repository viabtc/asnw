all:
	gcc test_rpc_svr.c ut_rpc_svr.c ut_rpc.c ut_crc32.c -I ../network/ -L ../network/ -lnetwork -lev -g -Wall -std=gnu99 -o test_rpc_svr
	gcc test_rpc_clt.c ut_rpc_clt.c ut_rpc.c ut_crc32.c -I ../network/ -L ../network/ -lnetwork -lev -g -Wall -std=gnu99 -o test_rpc_clt
