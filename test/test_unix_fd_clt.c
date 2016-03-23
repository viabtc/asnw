/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/21, create
 */

# include <stdio.h>  
# include <time.h>
# include <stdlib.h>  
# include <errno.h>  
# include <error.h>
# include <unistd.h>
# include <string.h>  
# include <fcntl.h>
# include <sys/types.h>
# include <sys/stat.h>  
# include <sys/socket.h>  
# include <sys/un.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: %s path file\n", argv[0]);
        exit(0);
    }

    int sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sockfd < 0)
        error(1, errno, "socket fail");

    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, argv[1], sizeof(un.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&un, sizeof(un)) < 0) {
        error(1, errno, "conect fail");
    }

    int fd = open(argv[2], 0);
    if (fd < 0) {
        error(1, errno, "open file fail");
    }

    struct msghdr msg;
    struct iovec io;
    char control[CMSG_SPACE(sizeof(int))];

    memset(&msg, 0, sizeof(msg));
    io.iov_base = &fd;
    io.iov_len = sizeof(fd);
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd;

    int ret = sendmsg(sockfd, &msg, 0);
    if (ret <= 0) {
        error(1, errno, "sendmsg fail");
    }

    char buf[10240];
    memset(&msg, 0, sizeof(msg));
    io.iov_base = buf;
    io.iov_len = sizeof(buf);
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    ret = recvmsg(sockfd, &msg, 0);
    if (ret < 0) {
        error(1, errno, "recvmsg fail");
    }

    buf[ret] = 0;
    printf("%s", buf);

    close(sockfd);

    return 0;
}

