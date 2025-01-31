#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/udp.h> // For UDP_SEGMENT
#include <getopt.h>      // For getopt
#include <time.h>

#define DEFAULT_SERVER_IP "10.1.90.3"
#define DEFAULT_SERVER_PORT 5001
#define DEFAULT_INNER_GSO_SIZE 1472
#define DEFAULT_GSO_SIZE (DEFAULT_INNER_GSO_SIZE * 6 + 8)
#define IP_HDR_SIZE 20
#define UDP_HDR_SIZE 8
#define MAX_UDP_PAYLOAD (UINT16_MAX - IP_HDR_SIZE - UDP_HDR_SIZE) // maximum UDP payload size
#define GSO_SEGMENT_SIZE (DEFAULT_INNER_GSO_SIZE * 6 + 8)

void create_fake_udp_header(char *buffer, size_t segment_size) {
    struct udphdr *fake_udp_header = (struct udphdr *)buffer;
    fake_udp_header->source = 0;  // Source port (not used, can be 0)
    fake_udp_header->dest = 0;    // Destination port (not used, can be 0)
    fake_udp_header->len = htons(segment_size); // Segment size
    fake_udp_header->check = 0;   // Checksum (optional, can be 0)
}

int main(int argc, char *argv[])
{
    int gso_segment_size = DEFAULT_GSO_SIZE;
    char *server_ip = DEFAULT_SERVER_IP;
    int server_port = DEFAULT_SERVER_PORT;
    int data_length = 0; // Data length for -l option
    int duration = 0;    // Duration for -t option

    // Option parsing
    int opt;
    struct option long_options[] = {
        {"gso", required_argument, 0, 'g'},
        {"ip", required_argument, 0, 'i'},
        {"port", required_argument, 0, 'p'},
        {"length", required_argument, 0, 'l'},
        {"time", required_argument, 0, 't'},
        {0, 0, 0, 0}};

    while ((opt = getopt_long(argc, argv, "g:i:p:l:t:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'g':
            gso_segment_size = atoi(optarg);
            break;
        case 'i':
            server_ip = optarg;
            break;
        case 'p':
            server_port = atoi(optarg);
            break;
        case 'l':
            data_length = atoi(optarg);
            break;
        case 't':
            duration = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s [--gso segment_size] [--ip server_ip] [--port server_port] [--length data_length] [--time duration]\n", argv[0]);
            exit(1);
        }
    }

    if (data_length == 0 && duration == 0)
    {
        fprintf(stderr, "Usage: %s [--gso segment_size] [--ip server_ip] [--port server_port] [--length data_length] [--time duration]\n", argv[0]);
        exit(1);
    }

    int sockfd;
    struct sockaddr_in server_addr;

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        perror("socket");
        exit(1);
    }

    // Enable UDP_SEGMENT option if -g is provided
    if (gso_segment_size > 0)
    {
        if (setsockopt(sockfd, SOL_UDP, UDP_SEGMENT, &gso_segment_size, sizeof(gso_segment_size)) == -1)
        {
            perror("setsockopt");
            close(sockfd);
            exit(1);
        }
        printf("UDP_SEGMENT option enabled with segment size: %d\n", gso_segment_size);
    }

    // Initialize server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_aton(server_ip, &server_addr.sin_addr) == 0)
    {
        fprintf(stderr, "Invalid IP address\n");
        close(sockfd);
        exit(1);
    }

    // Create a buffer for the message
    char *buffer = (char *)malloc(GSO_SEGMENT_SIZE + UDP_HDR_SIZE);
    if (buffer == NULL)
    {
        perror("malloc");
        close(sockfd);
        exit(1);
    }

    // Initialize the buffer with the fake UDP header and data
    create_fake_udp_header(buffer, DEFAULT_INNER_GSO_SIZE + UDP_HDR_SIZE);
    memset(buffer + UDP_HDR_SIZE, 'a', gso_segment_size - UDP_HDR_SIZE);

    // Initialize msghdr structure
    struct msghdr msg;
    struct iovec iov;
    char cmsgbuf[CMSG_SPACE(sizeof(uint16_t))];

    iov.iov_base = buffer;
    iov.iov_len = gso_segment_size;
    msg.msg_name = &server_addr;
    msg.msg_namelen = sizeof(server_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    *(uint16_t *)CMSG_DATA(cmsg) = gso_segment_size; // Segment size

    // Throughput measurement variables
    size_t total_bytes_sent = 0;
    time_t start_time = time(NULL);
    time_t current_time;

    // Set the ToS field to 0x01
    int tos = 0x01;
    if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == -1)
    {
        perror("setsockopt");
        close(sockfd);
        free(buffer);
        exit(1);
    }

    // Send the message in chunks
    if (data_length > 0)
    {
        int remaining_length = data_length;
        while (remaining_length > 0)
        {
            int send_length = (remaining_length > gso_segment_size - UDP_HDR_SIZE) ? gso_segment_size - UDP_HDR_SIZE : remaining_length;
            iov.iov_len = send_length + UDP_HDR_SIZE;

            if (sendmsg(sockfd, &msg, 0) == -1)
            {
                perror("sendmsg");
                close(sockfd);
                free(buffer);
                exit(1);
            }

            remaining_length -= send_length;
            total_bytes_sent += send_length + UDP_HDR_SIZE;

            current_time = time(NULL);
            if (current_time > start_time)
            {
                double seconds = difftime(current_time, start_time);
                double throughput = total_bytes_sent / seconds / 1024.0 / 1024.0 * 8.0; // Throughput in Mbps
                printf("Throughput: %.2f Mbps\n", throughput);
                total_bytes_sent = 0;
                start_time = current_time;
            }
        }

        printf("Total message of length %d bytes sent successfully.\n", data_length);
    }
    else if (duration > 0)
    {
        time_t end_time = start_time + duration;
        while (time(NULL) < end_time)
        {
            int send_length = GSO_SEGMENT_SIZE;
            iov.iov_len = send_length + UDP_HDR_SIZE;

            if (sendmsg(sockfd, &msg, 0) == -1)
            {
                perror("sendmsg");
                close(sockfd);
                free(buffer);
                exit(1);
            }

            total_bytes_sent += send_length;

            current_time = time(NULL);
            if (current_time > start_time)
            {
                double seconds = difftime(current_time, start_time);
                double throughput = total_bytes_sent / seconds / 1024.0 / 1024.0 * 8.0; // Throughput in Mbps
                printf("Throughput: %.2f Mbps\n", throughput);
                total_bytes_sent = 0;
                start_time = current_time;
            }
        }
    }

    // Cleanup
    close(sockfd);
    free(buffer);
    return 0;
}