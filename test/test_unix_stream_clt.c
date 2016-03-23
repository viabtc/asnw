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

    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
        error(1, errno, "socket fail");

    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, argv[1], sizeof(un.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&un, sizeof(un)) < 0) {
        error(1, errno, "conect fail");
    }

    char *line = NULL;
    size_t buf_size = 0;
    while (getline(&line, &buf_size, stdin) != -1) {
        int ret = write(sockfd, line, strlen(line));
        if (ret < 0) {
            error(1, errno, "write fail");
        }
        char buf[65535];
        ret = read(sockfd, buf, sizeof(buf));
        if (ret < 0) {
            error(1, errno, "read fail");
        }
        buf[ret] = 0;
        printf("%s", buf);
    }

    close(sockfd);

    return 0;
}

