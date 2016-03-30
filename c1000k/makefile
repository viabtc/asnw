all:
	gcc server.c -I ../../utils -L ../../utils -lutils -I ../../network/ -L ../../network/ -lnetwork -lev -std=gnu99 -Wall -g -o server
	gcc client.c -I ../../utils -L ../../utils -lutils -I ../../network/ -L ../../network/ -lnetwork -lev -std=gnu99 -Wall -g -o client

clean:
	rm server client
