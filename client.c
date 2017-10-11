#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>
#include <sys/time.h>

void error(const char *msg) {
    perror(msg);
    exit(0);
}

uint16_t fletcher16( uint8_t const *data, size_t bytes )
{
    uint16_t sum1 = 0xff, sum2 = 0xff;
    size_t tlen;

    while (bytes) {
        tlen = ((bytes >= 20) ? 20 : bytes);
        bytes -= tlen;
        do {
            sum2 += sum1 += *data++;
            tlen--;
        } while (tlen);
        sum1 = (sum1 & 0xff) + (sum1 >> 8);
        sum2 = (sum2 & 0xff) + (sum2 >> 8);
    }
    /* Second reduction step to reduce sums to 8 bits */
    sum1 = (sum1 & 0xff) + (sum1 >> 8);
    sum2 = (sum2 & 0xff) + (sum2 >> 8);
    return (sum2 << 8) | sum1;
}

struct package_recv{
    char type;
    int sq;
    char msg[256];
    struct package_recv *prev;
    struct package_recv *next;
};

struct package_recv_queue{
    struct package_recv *head;
    struct package_recv *tail;
    int size;
    unsigned long sum_sq;
};

struct package_sent{
    int type;
    int sq;
    char msg[256];
    long long timestamp;
    int received;
};

struct package_sent_queue{
    struct package_sent *arr[90000];
    int size;
};

void send_message(struct package_sent *item, int sockfd){
    char seq_str[256];
    char buffer[256];
    strcpy(buffer, item->msg);
    sprintf(seq_str, "%d", (item->type + item->sq));
    strcat(seq_str, buffer);
    strcpy(buffer, seq_str);
    uint16_t d = fletcher16((uint8_t const *)buffer, strlen(buffer));
    char check_digit[5];
    sprintf(check_digit, "%04x", d);
    char message[256];
    sprintf(message, "%s%s", check_digit, buffer);

    int n = write(sockfd,message,strlen(message));
    if (n < 0) {
        error("ERROR writing to socket");
    }
}

void resend(struct package_sent_queue queue, int sockfd){
    for(int i = 0; i < queue.size; i ++){
        if(queue.arr[i]->received != 1){
            send_message(queue.arr[i], sockfd);
        }
    }
}

void enqueue_sent(struct package_sent_queue *queue, struct package_sent *item){
    queue->arr[item->sq - 1] = item;
    queue->size ++;
}

void enqueue(struct package_recv_queue *queue, struct package_recv *item) {
    if(queue->size == 0){
        queue->head = item;
        queue->tail = item;
    }
    else{
        if(queue->tail->type == '9' && item->type == '9')
        return;
        struct package_recv *p = queue->tail;
        while(p != NULL){
            if(item->sq == p->sq){
                return;
            }
            else if(item->sq > p->sq){
                if(p == queue->tail){
                    item->prev = p;
                    queue->tail->next = item;
                    queue->tail = item;
                }
                else{
                    item->prev = p;
                    item->next = p->next;
                    p->next->prev = item;
                    p->next = item;
                }
                break;
            }
            p = p->prev;
        }
        if(p == NULL){
            queue->head->prev = item;
            item->next = queue->head;
            queue->head = item;
        }
    }
    if(item->type == '1'){
        queue->sum_sq += item->sq;
        queue->size ++;
    }
}

void print(struct package_recv_queue *queue) {
    struct package_recv *p;
    p = queue->head;
    while(p != NULL && p->type != '9'){
        printf("%s", p->msg);
        p = p->next;
    }
    p = queue->head;
    while(p != NULL){
        struct package_recv *tmp = p;
        p = p->next;
        free(tmp);
    }
}

ssize_t readLine(int fd, void *buffer, size_t n)
{
    ssize_t numRead;                    /* # of bytes fetched by last read() */
    size_t totRead;                     /* Total bytes read so far */
    char *buf;
    char ch;

    if (n <= 0 || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    buf = buffer;                       /* No pointer arithmetic on "void *" */

    totRead = 0;
    for (;;) {
        numRead = read(fd, &ch, 1);

        if (numRead == -1) {
            if (errno == EINTR)         /* Interrupted --> restart read() */
            continue;
            else
            return -1;              /* Some other error */
        }
        else if (numRead == 0) {      /* EOF */
            if (totRead == 0)           /* No bytes read; return 0 */
            return 0;
            else                        /* Some bytes read; add '\0' */
            break;

        }else {                        /* 'numRead' must be 1 if we get here */
            if (totRead < n - 1) {      /* Discard > (n - 1) bytes */
                totRead++;
                *buf++ = ch;
            }

            if (ch == '\n')
            break;
        }
    }

    *buf = '\0';
    return totRead;
}

int main(int argc, char *argv[]) {

    int sockfd, portno, n;
    struct sockaddr_in serv_addr;
    struct hostent *server;

    struct pollfd fds[2];  // {fd, events, revents}

    char buffer[256];
    int sequenceNumber = 0;

    int type_message  = 1000000;
    int type_ack      = 2000000;
    int type_nck      = 3000000;
    int type_timeout  = 4000000;
    int type_eof      = 9000000;

    int flag_all_sent = 0;
    int seq_num = 1;

    if (argc < 3) {
        fprintf(stderr,"usage %s hostname port\n", argv[0]);
        exit(0);
    }

    portno = atoi(argv[2]);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        error("ERROR opening socket");
    }

    server = gethostbyname(argv[1]);

    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host\n");
        exit(0);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
    (char *)&serv_addr.sin_addr.s_addr,
    server->h_length);
    serv_addr.sin_port = htons(portno);

    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
        error("ERROR connecting");
    }

    fds[0].fd = 0;
    fds[1].fd = sockfd;
    fds[0].events = POLLIN; //pollin because reading
    fds[1].events = POLLIN;

    struct package_recv_queue recv_queue;
    recv_queue.size = 0;
    recv_queue.sum_sq = 0;
    recv_queue.head = NULL;
    recv_queue.tail = NULL;

    struct package_sent_queue sent_queue;
    sent_queue.size = 0;

    while(1) {

        int r = poll(fds, 2, 2000);

        if(r == 0 && recv_queue.size != 0){ //send timeout signal
            struct package_sent item;
            item.type = type_timeout;
            strcpy(item.msg, "TIMEOUT\n");
            send_message(&item, sockfd);
        }

        if ((fds[0].revents & POLLIN) && !flag_all_sent) {

            bzero(buffer,256);
            if(fgets(buffer,255,stdin) != NULL){
                struct package_sent *item = (struct package_sent *)malloc(sizeof(struct package_sent));
                item->type = type_message;
                item->sq = seq_num;
                item->received = 0;
                strcpy(item->msg, buffer);

                send_message(item, sockfd);
                enqueue_sent(&sent_queue, item);
                seq_num ++;
            }
            else{
                struct package_sent *item = (struct package_sent *)malloc(sizeof(struct package_sent));
                item->type = type_eof;
                item->sq = seq_num;
                item->received = 0;
                strcpy(item->msg, "EOF\n");

                send_message(item, sockfd);
                enqueue_sent(&sent_queue, item);
                seq_num ++;
                flag_all_sent = 1;
            }

        }

        if (fds[1].revents & POLLIN) {

            bzero(buffer,256);
            char raw_data[256];
            char check_digit[5];
            n = readLine(sockfd, raw_data, 255);
            memcpy(check_digit, raw_data, 4);
            check_digit[4] = '\0';
            strcpy(buffer, raw_data + 4);
            uint16_t d = fletcher16((uint8_t const *)buffer, strlen(buffer));

            if(d != (uint16_t) strtol(check_digit, NULL, 16)){
                continue;
            }

            if (n < 0) {
                error("ERROR reading from socket");
            }else if (n == 0){
                break;
            }

            char data_type = buffer[0];
            char sq[7];
            memcpy(sq, buffer + 1, 6);
            sq[6] = '\0';

            if(data_type == '1' || data_type == '9'){
                struct package_recv *p = (struct package_recv *)malloc(sizeof(struct package_recv));;
                p->type = buffer[0];
                p->sq = atoi(sq);
                p->prev = NULL;
                p->next = NULL;
                strcpy(p->msg, buffer + 7);
                enqueue(&recv_queue, p);

                char response[16];
                sprintf(response, "%dACK\n", type_ack + p->sq);
                uint16_t d = fletcher16((uint8_t const *)response, strlen(response));
                char check_digit[5];
                sprintf(check_digit, "%04x", d);
                char message[256];
                sprintf(message, "%s%s", check_digit, response);
                write(sockfd, message, strlen(message));
            }
            else if(data_type == '2'){
                sent_queue.arr[atoi(sq) - 1]->received = 1;
            }
            else if(data_type == '4'){
                resend(sent_queue, sockfd);
            }

            if(recv_queue.size !=0 && recv_queue.tail->type == '9' && recv_queue.sum_sq == (long)recv_queue.tail->sq * (recv_queue.tail->sq - 1) / 2 ){
                print(&recv_queue);
                break;
            }
        }
    }

    close(sockfd);
    for(int i = 0; i < sent_queue.size; i ++){
        free(sent_queue.arr[i]);
    }
    return 0;
}
