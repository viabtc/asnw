/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/21, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <unistd.h>
# include <error.h>
# include <errno.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h> 

# include "ut_misc.h"

int main(int argc, char *argv[])
{
    set_file_limit(1000000);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_aton("127.0.0.1", &addr.sin_addr);

    int max = 100;
    int *fds = malloc(max * 10000 * sizeof(int));
    int connections = 0;
    for (int i = 0; i < max; ++i) {
        addr.sin_port = htons(5000 + i);
        for (int j = 0; j < 10000; ++j) {
            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                goto error;
            }
            if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                goto error;
            }
            fds[10000*i+j] = sockfd;

            usleep(100);
            connections += 1;
            if (connections % 1000 == 0) {
                printf("connections: %d\n", connections);
            }
        }
    }

    printf("connections: %d\n", connections);
    sleep(10);

    for (int i = 0; i < max * 10000; ++i)
        close(fds[i]);

    return 0;

error:
    printf("connections: %d, error: %s\n", connections, strerror(errno));
    return 0;
}

