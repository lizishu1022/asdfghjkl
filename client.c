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

void error(const char *msg) {
    perror(msg);
    exit(0);
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
            //printf("comparing item.sq: %d, p.sq %d\n",item->sq , p->sq );
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
    //while(p != NULL){
        printf("%s", p->msg);
        p = p->next;
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

        } else if (numRead == 0) {      /* EOF */
            if (totRead == 0)           /* No bytes read; return 0 */
                return 0;
            else                        /* Some bytes read; add '\0' */
                break;

        } else {                        /* 'numRead' must be 1 if we get here */
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

    while(1) {

        poll(fds, 2, 1000);

        if (fds[0].revents && POLLIN && !flag_all_sent) {

            bzero(buffer,256);
            if(fgets(buffer,255,stdin) != NULL){
                char seq_str[256];
                sprintf(seq_str, "%d", (type_message + seq_num));
                strcat(seq_str, buffer);
                strcpy(buffer, seq_str);
                seq_num++;

                n = write(sockfd,buffer,strlen(buffer));
                if (n < 0) {
                    error("ERROR writing to socket");
                }
            }
            else{
                char seq_str[256];
                sprintf(seq_str, "%d\n", (type_eof + seq_num));
                seq_num++;

                n = write(sockfd,seq_str,strlen(seq_str));
                if (n < 0) {
                    error("ERROR writing to socket");
                }
                flag_all_sent = 1;
            }

        }

        if (fds[1].revents && POLLIN) {

            bzero(buffer,256);
            n = readLine(sockfd, buffer, 255);


            //提取这个buffer中的sequence number，在标识符":"之前的string，就是sequence number.
            //提取之后，sequence number 是一个char[]，把它转换为int.
            //定义两个新的buffer，bufferA - 这里放的是in order的； bufferB - 这里放的是非order的.
            //第一个sequence number 应该是1，如果是，把sequence number 和标识符":"都remove掉，然后放入bufferA.如果不是，放入bufferB。
            //然后陆续传入sequence number，根据bufferA里已经存放的那些number来判断应该放入bufferA还是bufferB。 假如传入的这个number是8，那就要去对比bufferA里是否1-7都有了。 （这一步骤应该想想有没有更高效的方法，本质上这应该是个排序算法。）


            if (n < 0) {
                error("ERROR reading from socket");
            }
            if (n == 0) {
                break;
            }
            char data_type = buffer[0];
            char sq[8];
            memcpy(sq, buffer + 1, 7);
            sq[7] = '\0';

            struct package_recv *p = (struct package_recv *)malloc(sizeof(struct package_recv));;
            p->type = buffer[0];
            p->sq = atoi(sq);
            p->prev = NULL;
            p->next = NULL;

            //printf("%s", buffer);

            if(data_type == '1'){
              strcpy(p->msg, buffer + 7);
            }else if(data_type == '9'){
              strcpy(p->msg, "EOF\n");
            }
            enqueue(&recv_queue, p);
            //print(&recv_queue);
            //printf("%c\n", recv_queue.tail->type);
            //printf("%d\n", recv_queue.size);
            if(recv_queue.tail->type == '9' && recv_queue.sum_sq == (long)recv_queue.tail->sq * (recv_queue.tail->sq - 1) / 2 ){
                print(&recv_queue);
                break;
            }
        }
    }

    close(sockfd);
    return 0;
}
