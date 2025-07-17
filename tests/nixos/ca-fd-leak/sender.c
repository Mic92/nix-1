#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

int main(int argc, char **argv) {

    assert(argc == 2);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // Set up a abstract domain socket path to connect to.
    struct sockaddr_un data;
    data.sun_family = AF_UNIX;
    data.sun_path[0] = 0;
    strncpy(data.sun_path + 1, argv[1], sizeof(data.sun_path) - 2);

    // Now try to connect, To ensure we work no matter what order we are
    // executed in, just busyloop here.
    int res = -1;
    while (res < 0) {
        res = connect(sock, (const struct sockaddr *)&data,
            offsetof(struct sockaddr_un, sun_path)
              + strlen(argv[1])
              + 1);
        if (res < 0) {
            if (errno != ECONNREFUSED) {
                perror("connect");
                break;
            }
        } else {
            break;
        }
    }

    // Write our message header.
    struct msghdr msg = {0};
    msg.msg_control = malloc(128);
    if (!msg.msg_control) {
        perror("malloc");
        return 1;
    }
    msg.msg_controllen = 128;

    // Write an SCM_RIGHTS message containing the output path.
    struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg);
    hdr->cmsg_len = CMSG_LEN(sizeof(int));
    hdr->cmsg_level = SOL_SOCKET;
    hdr->cmsg_type = SCM_RIGHTS;
    const char *out_path = getenv("out");
    if (!out_path) {
        fprintf(stderr, "Environment variable 'out' not set\n");
        free(msg.msg_control);
        return 1;
    }
    int fd = open(out_path, O_RDWR | O_CREAT, 0640);
    if (fd < 0) {
        perror("open");
        free(msg.msg_control);
        return 1;
    }
    memcpy(CMSG_DATA(hdr), (void *)&fd, sizeof(int));

    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    // Write a single null byte too.
    msg.msg_iov = (struct iovec*) malloc(sizeof(struct iovec));
    if (!msg.msg_iov) {
        perror("malloc");
        free(msg.msg_control);
        close(fd);
        return 1;
    }
    msg.msg_iov[0].iov_base = (void*) "";
    msg.msg_iov[0].iov_len = 1;
    msg.msg_iovlen = 1;

    // Send it to the othher side of this connection.
    res = sendmsg(sock, &msg, 0);
    if (res < 0) perror("sendmsg");
    int buf;

    // Wait for the server to close the socket, implying that it has
    // received the commmand.
    recv(sock, (void *)&buf, sizeof(int), 0);

    // Clean up
    free(msg.msg_control);
    free(msg.msg_iov);
    close(fd);
    close(sock);
}
