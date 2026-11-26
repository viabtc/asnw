all:
	gcc server.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu99 -Wall -g -o server
	gcc client.c -I ../asnw/ -L ../asnw/ -lasnw -lev -lpthread -std=gnu99 -Wall -g -o client

clean:
	rm server client
