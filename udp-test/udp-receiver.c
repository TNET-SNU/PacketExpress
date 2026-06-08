#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/udp.h> // For UDP_GRO
#include <getopt.h>      // For getopt
#include <time.h>

#define MAX_BUF_SIZE 131072 /* one caravan is a single outer UDP datagram (<=64K),
                             * but RX-GRO may coalesce several outer caravans into
                             * one skb, so allow headroom and watch MSG_TRUNC. */
#define DEFAULT_PORT 5001
#define DEFAULT_IP "0.0.0.0"
#define MAX_MSG_COUNT 64 // Maximum number of messages per recvmmsg

int main(int argc, char *argv[])
{
    /* Unbuffered stdout so dumps appear immediately even through a pipe
     * (e.g. `| grep`); otherwise the last message can sit in the 4K block
     * buffer until the next recvmsg() unblocks. */
    setvbuf(stdout, NULL, _IONBF, 0);

    int port = DEFAULT_PORT;
    char *ip = DEFAULT_IP;
    int enable_gro = 0;  // Flag for enabling UDP_GRO
    int dump_output = 0; // Flag for enabling output dump
    int use_mmsg = 0;    // Flag: use recvmmsg() instead of recvmsg()

    // Option parsing
    int opt;
    struct option long_options[] = {
        {"gro", no_argument, 0, 'g'},
        {"port", required_argument, 0, 'p'},
        {"ip", required_argument, 0, 'i'},
        {"dump", no_argument, 0, 'd'},
        {"mmsg", no_argument, 0, 'm'},
        {0, 0, 0, 0}};

    while ((opt = getopt_long(argc, argv, "gp:i:dm", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'p':
            port = atoi(optarg);
            break;
        case 'i':
            ip = optarg;
            break;
        case 'g':
            enable_gro = 1;
            break;
        case 'd':
            dump_output = 1;
            break;
        case 'm':
            use_mmsg = 1;
            break;
        default:
            fprintf(stderr, "Usage: %s [--gro] [--dump] [--mmsg] [-p port] [-i ip]\n", argv[0]);
            exit(1);
        }
    }

    if (port <= 1024 || port > 65535)
    {
        fprintf(stderr, "Invalid port number\n");
        exit(1);
    }

    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        perror("socket");
        exit(1);
    }

    // Enable UDP_GRO option if requested
    if (enable_gro)
    {
        int enable = 1;
        if (setsockopt(sockfd, IPPROTO_UDP, UDP_GRO, &enable, sizeof(enable)) == -1)
        {
            perror("setsockopt");
            close(sockfd);
            exit(1);
        }
        if (dump_output)
        {
            printf("UDP_GRO option enabled\n");
        }
    }

    // Increase buffer size
    // int rcvbuf_size = 1000000000;
    // if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) == -1)
    // {
    //     perror("setsockopt SO_RCVBUF");
    //     exit(1);
    // }

    // Initialize server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    // Bind the socket
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(sockfd);
        exit(1);
    }

    if (dump_output)
    {
        printf("UDP receiver is waiting for incoming packets on %s:%d...\n", ip, port);
    }

    // --- recvmsg (single-message) state ---
    char buffer[MAX_BUF_SIZE + 1]; // +1 for null terminator on dump
    ssize_t recv_len;
    struct msghdr msg;
    struct iovec iov;
    char cmsgbuf[CMSG_SPACE(sizeof(int))]; /* kernel sends gso_size as int (4B) */

    iov.iov_base = buffer;
    iov.iov_len = MAX_BUF_SIZE;
    msg.msg_name = &client_addr;
    msg.msg_namelen = sizeof(client_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    // --- recvmmsg (batched) state; per-slot buffers allocated only when used ---
    struct mmsghdr msgs[MAX_MSG_COUNT];
    struct iovec iovecs[MAX_MSG_COUNT];
    char *buffers[MAX_MSG_COUNT] = {0};
    struct sockaddr_in addrs[MAX_MSG_COUNT];
    char ctrls[MAX_MSG_COUNT][CMSG_SPACE(sizeof(int))]; // per-msg UDP_GRO cmsg

    if (use_mmsg)
    {
        for (int i = 0; i < MAX_MSG_COUNT; i++)
        {
            buffers[i] = calloc(MAX_BUF_SIZE + 1, 1); // +1 for null terminator on dump
            memset(&msgs[i], 0, sizeof(struct mmsghdr));
            iovecs[i].iov_base = buffers[i];
            iovecs[i].iov_len = MAX_BUF_SIZE;
            msgs[i].msg_hdr.msg_iov = &iovecs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = &addrs[i];          // sender address
            msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
            msgs[i].msg_hdr.msg_control = ctrls[i];        // for UDP_GRO gso_size
            msgs[i].msg_hdr.msg_controllen = sizeof(ctrls[i]);
        }
    }

    // Throughput measurement variables
    size_t total_bytes = 0;
    time_t start_time = time(NULL);

    // Receive data from clients
    while (1)
    {
      if (!use_mmsg)
      {
        /* recvmsg() overwrites these with the lengths it actually used, so
         * they must be restored before every call -- otherwise, once a
         * packet without control data sets msg_controllen to 0, the GRO
         * gso_size cmsg is silently never delivered again. */
        msg.msg_namelen    = sizeof(client_addr);
        msg.msg_controllen = sizeof(cmsgbuf);
        msg.msg_flags      = 0;

        recv_len = recvmsg(sockfd, &msg, 0);
        if (recv_len == -1)
        {
            perror("recvmsg");
            close(sockfd);
            exit(1);
        }

        if (msg.msg_flags & MSG_TRUNC)
            fprintf(stderr, "WARN: datagram truncated (got %zd, buffer=%d) -- enlarge MAX_BUF_SIZE\n",
                    recv_len, MAX_BUF_SIZE);
        if (msg.msg_flags & MSG_CTRUNC)
            fprintf(stderr, "WARN: control data truncated -- gso_size cmsg may be missing\n");

        // Update total bytes received
        total_bytes += recv_len;

        struct cmsghdr *cmsg;
        int each_size;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_UDP && cmsg->cmsg_type == UDP_GRO)
            {
                each_size = *(int *)CMSG_DATA(cmsg);
                if (dump_output)
                {
                    printf("each_size: %d\n", each_size);
                }
            }
        }

        if (dump_output)
        {
            // Null-terminate the received data
            buffer[recv_len] = '\0';
            printf("Received message of size %ld from %s:%d: %s\n", recv_len, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), buffer);
        }
      }
      else
      {
        /* recvmmsg overwrites msg_namelen/msg_controllen/msg_flags per
         * message, so restore them before every call (same reason as the
         * recvmsg path -- otherwise the UDP_GRO cmsg stops being delivered). */
        for (int i = 0; i < MAX_MSG_COUNT; i++)
        {
            msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
            msgs[i].msg_hdr.msg_controllen = sizeof(ctrls[i]);
            msgs[i].msg_hdr.msg_flags = 0;
        }

        int num_msgs = recvmmsg(sockfd, msgs, MAX_MSG_COUNT, 0, NULL);
        if (num_msgs == -1)
        {
            perror("recvmmsg");
            close(sockfd);
            exit(1);
        }

        for (int i = 0; i < num_msgs; i++)
        {
            struct msghdr *h = &msgs[i].msg_hdr;
            total_bytes += msgs[i].msg_len;

            if (h->msg_flags & MSG_TRUNC)
                fprintf(stderr, "WARN: msg %d truncated (got %u, buffer=%d)\n",
                        i, msgs[i].msg_len, MAX_BUF_SIZE);

            // Per-message UDP_GRO gso_size, if present
            int each_size = 0, have_gro = 0;
            for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(h); cmsg != NULL;
                 cmsg = CMSG_NXTHDR(h, cmsg))
            {
                if (cmsg->cmsg_level == SOL_UDP && cmsg->cmsg_type == UDP_GRO)
                {
                    each_size = *(int *)CMSG_DATA(cmsg);
                    have_gro = 1;
                }
            }

            if (dump_output)
            {
                struct sockaddr_in *ca = (struct sockaddr_in *)h->msg_name;
                if (have_gro)
                    printf("each_size: %d\n", each_size);
                buffers[i][msgs[i].msg_len] = '\0';
                printf("Received message of size %u from %s:%d: %s\n",
                       msgs[i].msg_len,
                       inet_ntoa(ca->sin_addr),
                       ntohs(ca->sin_port),
                       buffers[i]);
            }
        }
      }

        // Throughput measurement and output every second
        time_t current_time = time(NULL);
        if (current_time > start_time)
        {
            double seconds = difftime(current_time, start_time);
            double throughput = total_bytes / seconds / 1000.0 / 1000.0 * 8.0; // Throughput in Mbps
            printf("Throughput: %.2f Mbps\n", throughput);
            total_bytes = 0;
            start_time = current_time;
        }
    }

    close(sockfd);
    return 0;
}

