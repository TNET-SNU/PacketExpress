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
#include <sys/stat.h>    // For fstat (socket inode)

#define MAX_BUF_SIZE 131072 /* one caravan is a single outer UDP datagram (<=64K),
                             * but RX-GRO may coalesce several outer caravans into
                             * one skb, so allow headroom and watch MSG_TRUNC. */
#define DEFAULT_PORT 5001
#define DEFAULT_IP "0.0.0.0"
#define MAX_MSG_COUNT 64 // Maximum number of messages per recvmmsg

/* This socket's sk_drops (app-side loss, incl. RcvbufErrors) and current
 * rx_queue bytes, read from /proc/net/udp matched by socket inode. */
static void sock_stats(unsigned long ino, unsigned long *drops, unsigned long *rxq)
{
    *drops = 0;
    *rxq = 0;
    FILE *f = fopen("/proc/net/udp", "r");
    if (!f)
        return;
    char line[512];
    if (fgets(line, sizeof(line), f)) { /* skip header */ }
    while (fgets(line, sizeof(line), f))
    {
        unsigned int rq, dr;
        unsigned long inode;
        /* sl: laddr:lport raddr:rport st tx:rx tr:tm retr uid to inode ref ptr drops */
        if (sscanf(line,
                   "%*d: %*x:%*x %*x:%*x %*x %*x:%x %*x:%*x %*x %*x %*x %lu %*x %*s %u",
                   &rq, &inode, &dr) == 3 && inode == ino)
        {
            *rxq = rq;
            *drops = dr;
            break;
        }
    }
    fclose(f);
}

/* Read a NIC counter from /sys/class/net/<iface>/statistics/<name>. */
static unsigned long nic_stat(const char *iface, const char *name)
{
    char path[256];
    unsigned long v = 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", iface, name);
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fscanf(f, "%lu", &v) != 1)
            v = 0;
        fclose(f);
    }
    return v;
}

static unsigned long nic_drops(const char *iface)
{
    if (!iface)
        return 0;
    return nic_stat(iface, "rx_dropped") + nic_stat(iface, "rx_missed_errors") +
           nic_stat(iface, "rx_fifo_errors") + nic_stat(iface, "rx_over_errors") +
           nic_stat(iface, "rx_nohandler");
}

/* Binary-safe dump: printable bytes as-is, others as '.'.  Needed because in
 * PX-aware mode the buffer contains binary inner UDP headers (NUL bytes) that
 * would truncate a plain "%s" print.  Whole message is printed (use --dump only
 * at low rate). */
static void dump_buf(const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        putchar((buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '.');
}

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
    char *iface = NULL;  // optional NIC for /sys drop counters

    // Option parsing
    int opt;
    struct option long_options[] = {
        {"gro", no_argument, 0, 'g'},
        {"port", required_argument, 0, 'p'},
        {"ip", required_argument, 0, 'i'},
        {"dump", no_argument, 0, 'd'},
        {"mmsg", no_argument, 0, 'm'},
        {"iface", required_argument, 0, 'I'},
        {0, 0, 0, 0}};

    while ((opt = getopt_long(argc, argv, "gp:i:dmI:", long_options, NULL)) != -1)
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
        case 'I':
            iface = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s [--gro] [--dump] [--mmsg] [--iface <nic>] [-p port] [-i ip]\n", argv[0]);
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

    /* Enlarge the socket receive buffer to absorb bursts / scheduling jitter
     * so packets aren't dropped (counted as UdpRcvbufErrors) while CPU has
     * headroom.  SO_RCVBUFFORCE (needs root/CAP_NET_ADMIN) bypasses the
     * net.core.rmem_max cap; fall back to SO_RCVBUF otherwise. */
    int rcvbuf_size = 256 << 20; /* 256 MB */
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf_size, sizeof(rcvbuf_size)) == -1)
    {
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) == -1)
            perror("setsockopt SO_RCVBUF");
    }
    {
        int actual = 0;
        socklen_t alen = sizeof(actual);
        if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &actual, &alen) == 0)
            fprintf(stderr, "SO_RCVBUF = %d bytes (kernel-reported, usually 2x requested)\n",
                    actual);
    }

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

    /* Loss-monitor state: this socket's inode + baseline drop counters. */
    unsigned long my_ino = 0;
    struct stat sst;
    if (fstat(sockfd, &sst) == 0)
        my_ino = (unsigned long)sst.st_ino;
    unsigned long prev_sock_drops = 0, sock_rxq = 0;
    sock_stats(my_ino, &prev_sock_drops, &sock_rxq);
    unsigned long prev_nic_drops = nic_drops(iface);

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
            printf("Received message of size %ld from %s:%d: ", recv_len,
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            dump_buf((unsigned char *)buffer, recv_len);
            putchar('\n');
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

        /* MSG_WAITFORONE: block for the first datagram, then drain whatever
         * is already queued and return -- otherwise recvmmsg() blocks until
         * all MAX_MSG_COUNT slots fill, which never happens at low rate. */
        int num_msgs = recvmmsg(sockfd, msgs, MAX_MSG_COUNT, MSG_WAITFORONE, NULL);
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
                printf("Received message of size %u from %s:%d: ",
                       msgs[i].msg_len, inet_ntoa(ca->sin_addr), ntohs(ca->sin_port));
                dump_buf((unsigned char *)buffers[i], msgs[i].msg_len);
                putchar('\n');
            }
        }
      }

        // Throughput measurement and output every second
        time_t current_time = time(NULL);
        if (current_time > start_time)
        {
            double seconds = difftime(current_time, start_time);
            double throughput = total_bytes / seconds / 1000.0 / 1000.0 * 8.0; // Throughput in Mbps

            /* App-side (socket) loss + current backlog, from /proc/net/udp. */
            unsigned long sock_drops = 0;
            sock_stats(my_ino, &sock_drops, &sock_rxq);
            unsigned long sdps = (sock_drops - prev_sock_drops) / (unsigned long)seconds;
            prev_sock_drops = sock_drops;

            unsigned long ndps = 0;
            if (iface)
            {
                unsigned long nd = nic_drops(iface);
                ndps = (nd - prev_nic_drops) / (unsigned long)seconds;
                prev_nic_drops = nd;
            }

            printf("Throughput: %.2f Mbps | sock drops/s: %lu (total %lu) | rxq: %lu B",
                   throughput, sdps, sock_drops, sock_rxq);
            if (iface)
                printf(" | nic drops/s: %lu", ndps);
            printf("%s\n", (sdps || ndps) ? "   <== LOSS" : "");

            total_bytes = 0;
            start_time = current_time;
        }
    }

    close(sockfd);
    return 0;
}
