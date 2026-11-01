// Read-only on-device RetroArch protocol probe for overlay smoke tests.
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(55355), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct timeval timeout = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    if (sendto(fd, "GET_STATUS", 10, 0, (void *)&addr, sizeof addr) < 0) return 1;
    char reply[2048];
    ssize_t n = recvfrom(fd, reply, sizeof reply - 1, 0, NULL, NULL);
    gettimeofday(&t1, NULL);
    close(fd);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
    fprintf(stderr, "ra-status: %s after %ld ms\n", n < 0 ? "no answer" : "answered", ms);
    if (n < 0) { perror("GET_STATUS"); return 1; }
    reply[n] = 0;
    printf("%s", reply);
    if (argc > 1) {
        char state[32];
        if (sscanf(reply, "GET_STATUS %31s", state) != 1 || strcmp(state, argv[1])) return 1;
    }
    return 0;
}
