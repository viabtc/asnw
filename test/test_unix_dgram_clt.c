/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/21, create
 */

# include <stdio.h>  
# include <stdlib.h>  
# include <errno.h>  
# include <error.h>
# include <unistd.h>
# include <string.h>  
# include <sys/stat.h>  
# include <sys/socket.h>  
# include <sys/un.h>  

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("usage: %s path\n", argv[0]);
        exit(0);
    }

    int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error(1, errno, "socket fail");

    struct sockaddr_un client, server;
    memset(&client, 0, sizeof(client));
    client.sun_family = AF_UNIX;
    strncpy(client.sun_path, tmpnam(NULL), sizeof(client.sun_path) - 1);
    unlink(client.sun_path);
    if (bind(sockfd, (struct sockaddr *)&client, sizeof(client)) < 0) {
        error(1, errno, "bind fail");
    }

    memset(&server, 0, sizeof(server));
    server.sun_family = AF_UNIX;
    strncpy(server.sun_path, argv[1], sizeof(server.sun_path) - 1);

    char *line = NULL;
    size_t buf_size = 0;
    while (getline(&line, &buf_size, stdin) != -1) {
        int ret = sendto(sockfd, line, strlen(line), 0, (struct sockaddr *)&server, sizeof(server));
        if (ret < 0) {
            error(1, errno, "sendto fail");
        }
        char buf[65535];
        ret = recvfrom(sockfd, buf, sizeof(buf), 0, NULL, NULL);
        if (ret < 0) {
            error(1, errno, "recvfrom fail");
        }
        buf[ret] = 0;
        printf("%s", buf);
    }

    return 0;
}

