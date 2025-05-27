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
            int err = errno; // Save errno value immediately after connect
            if (err != ECONNREFUSED) {
                perror("connect");
                break;
            }
        } else {
            break; // Connection succeeded
        }
    }

    // Write our message header.
    struct msghdr msg = {0};
    msg.msg_control = malloc(128);
    if (!msg.msg_control) {
        perror("malloc");
        close(sock);
        return 1;
    }
    msg.msg_controllen = 128;

    // Write an SCM_RIGHTS message containing the output path.
    struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg);
    if (!hdr) {
        fprintf(stderr, "Failed to get control message header\n");
        close(sock);
        free(msg.msg_control);
        return 1;
    }

    hdr->cmsg_len = CMSG_LEN(sizeof(int));
    hdr->cmsg_level = SOL_SOCKET;
    hdr->cmsg_type = SCM_RIGHTS;

    // Check if environment variable exists before using it
    const char* out_path = getenv("out");
    if (!out_path) {
        fprintf(stderr, "Environment variable 'out' not set\n");
        close(sock);
        free(msg.msg_control);
        return 1;
    }

    int fd = open(out_path, O_RDWR | O_CREAT, 0640);
    if (fd < 0) {
        perror("open");
        free(msg.msg_control);
        close(sock);
        return 1;
    }

    memcpy(CMSG_DATA(hdr), (void *)&fd, sizeof(int));

    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    // Write a single null byte too.
    msg.msg_iov = (struct iovec*) malloc(sizeof(struct iovec));
    if (!msg.msg_iov) {
        perror("malloc");
        close(fd);
        close(sock);
        free(msg.msg_control);
        return 1;
    }

    msg.msg_iov[0].iov_base = (void*) "";
    msg.msg_iov[0].iov_len = 1;
    msg.msg_iovlen = 1;

    // Send it to the other side of this connection.
    res = sendmsg(sock, &msg, 0);
    if (res < 0) {
        perror("sendmsg");
        // Continue execution - we'll try to receive anyway
    }
    int buf;

    // Wait for the server to close the socket, implying that it has
    // received the command.
    int recv_result = recv(sock, (void *)&buf, sizeof(int), 0);
    if (recv_result < 0) {
        perror("recv");
        // Continue to cleanup
    }

    // Clean up resources
    close(fd);
    close(sock);
    free(msg.msg_control);
    free(msg.msg_iov);

    return 0;
}
