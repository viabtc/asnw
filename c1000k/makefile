all:
	gcc server.c -I ../network/ -L ../network/ -lnetwork -lev -std=gnu99 -Wall -g -o server
	gcc client.c -I ../network/ -L ../network/ -lnetwork -lev -std=gnu99 -Wall -g -o client

clean:
	rm server client
