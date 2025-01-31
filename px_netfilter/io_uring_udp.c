#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 12345
#define BUFFER_SIZE 2048
#define QUEUE_DEPTH 8

int main() {
    struct io_uring ring;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int udp_sock;
    char buffer[BUFFER_SIZE];
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    // UDP 소켓 생성
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket");
        return 1;
    }

    // 소켓 주소 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    // 바인딩
    if (bind(udp_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(udp_sock);
        return 1;
    }

    // io_uring 초기화
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        close(udp_sock);
        return 1;
    }

    // Zero-copy 데이터 수신 요청 등록
    for (int i = 0; i < QUEUE_DEPTH; i++) {
        sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            fprintf(stderr, "Failed to get submission queue entry\n");
            break;
        }

        io_uring_prep_recvmsg(sqe, udp_sock, NULL, 0);
        sqe->flags |= IOSQE_ZEROCOPY;
        io_uring_sqe_set_data(sqe, buffer);
    }

    if (io_uring_submit(&ring) < 0) {
        perror("io_uring_submit");
        io_uring_queue_exit(&ring);
        close(udp_sock);
        return 1;
    }

    printf("Waiting for UDP packets on port %d...\n", PORT);

    // 이벤트 처리 루프
    while (1) {
        // Complete Queue에서 이벤트 확인
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            perror("io_uring_wait_cqe");
            break;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Error receiving data: %s\n", strerror(-cqe->res));
        } else {
            printf("Received packet: %.*s\n", cqe->res, (char *)io_uring_cqe_get_data(cqe));
        }

        // 완료된 이벤트 반환
        io_uring_cqe_seen(&ring, cqe);

        // 새로운 요청 등록
        sqe = io_uring_get_sqe(&ring);
        if (sqe) {
            io_uring_prep_recvmsg(sqe, udp_sock, NULL, 0);
            sqe->flags |= IOSQE_ZEROCOPY;
            io_uring_sqe_set_data(sqe, buffer);
            if (io_uring_submit(&ring) < 0) {
                perror("io_uring_submit");
                break;
            }
        }
    }

    // 정리
    io_uring_queue_exit(&ring);
    close(udp_sock);

    return 0;
}
