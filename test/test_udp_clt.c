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

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: %s ip port\n", argv[0]);
        exit(0);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        error(1, errno, "create socket fail");
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_aton(argv[1], &addr.sin_addr) == 0) {
        error(1, errno, "ip error");
    }
    addr.sin_port = htons(atoi(argv[2]));

    char *line = NULL;
    size_t buf_size = 0;
    while (getline(&line, &buf_size, stdin) != -1) {
        int ret = sendto(sockfd, line, strlen(line), 0, (struct sockaddr *)&addr, sizeof(addr));
        if (ret < 0) {
            error(1, errno, "sendto fail\n");
        }

        char buf[10240];
        ret = recvfrom(sockfd, buf, sizeof(buf), 0, NULL, NULL);
        if (ret < 0) {
            error(1, errno, "recvfrom fail\n");
        }
        buf[ret] = 0;
        printf("%s", buf);
    }

    close(sockfd);

    return 0;
}

