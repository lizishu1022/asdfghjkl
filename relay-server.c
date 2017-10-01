/*****************************************************************************
   Unreliable Relay Server
   
   Listens for two client connections, and then forwards incoming traffic
   from each to the other - unreliably!
  
   The purpose is to provide an environment for students to test their own reliability
   implementation code in clients.  Some input messages may be forwarded out-of-order,
   late, mangled, duplicated, ... or not at all.  Student clients need to implement a
   reliability protocol at the application layer to overcome these hurdles effectively,
   and hopefully also efficiently.

   Written by Tim Seeley, Sep 2015, drawing initially upon
   http://www.linuxhowtos.org/data/6/server.c
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
 
#include "urs-util.h"

#define BUFSIZE 128
#define OUT stderr

//#define DEBUG

/* internal function headers */
int enqueue_message(int q);
int send_message(int sq);
void randomly_corrupt(char *msg);
void corrupt_character_flip(char *msg);
void corrupt_insert_newline(char *msg);
void corrupt_truncate_clean(char *msg);
void corrupt_truncate_clean(char *msg);
void corrupt_truncate_dirty(char *msg);

/* option flags set from cmd-line option args */
int flag_verbose = 0;
int flag_drop = 0;
int flag_corrupt_rate = 0;
int flag_corrupt_type = 1;
int flag_latency = 0;
int flag_reorder_rate = 0;
int flag_reorder_step = 0; // default 0 => randomised
int flag_duplicate_rate = 0;

/* global stuff because I'm slack */
int sessionsockfd[2];    // sockets
char buffer[2][BUFSIZE]; // read buffers for each socket
int buf_insert[2];       // insertion indexes for each read buffer
int msgcount[2];         // message counters for each input channel
struct mq* msq[2];       // message send queues for each output channel
int client_bytes[2];     // counts of incoming bytes from each client
long long client_start[2];  // time of first incoming message from each client
long long client_latest[2]; // time of most recent incoming message from each client


int main(int argc, char *argv[]){
  int welcomesockfd, port;
  socklen_t clilen;
  struct sockaddr_in serv_addr, cli_addr[2];
  int n,c;
  struct pollfd poll_array[2];

  /* process command-line arguments */
  while ((c = getopt(argc, argv, "c:C:dl:r:R:vx:h")) != -1){
    switch(c){
      case 'c':
        flag_corrupt_rate = atoi(optarg);
	break;
      case 'C':
        flag_corrupt_type = atoi(optarg);
	break;
      case 'd':
        flag_drop++;
        break;
      case 'l':
        flag_latency = atoi(optarg);
	break;
      case 'r':
        flag_reorder_rate = atoi(optarg);
        break;
      case 'R':
        flag_reorder_step = atoi(optarg);
        break;
      case 'v':
        flag_verbose++;
        break;
      case 'x':
        flag_duplicate_rate = atoi(optarg);
	break;
      case 'h':
        fprintf(stderr,"Usage: %s [options] port\n", argv[0]);
        fprintf(stderr,"Currently supported options:\n");
        fprintf(stderr," -c p  Randomly corrupt about p%% of messages.\n");
        fprintf(stderr," -C t  corruption type: 1=char-flip; 2=insert-newline; 3=truncate; ...\n");
        fprintf(stderr," -d    Randomly drop about 10%% of messages.\n");
        fprintf(stderr," -dd   Randomly drop about 25%% of messages.\n");
        fprintf(stderr," -ddd  Randomly drop about 50%% of messages.\n");
        fprintf(stderr," -l m  Add at least m milliseconds latency to each message.\n");
        fprintf(stderr," -r p  Reorder about p%% of messages according to -R setting.\n");
        fprintf(stderr," -R p  Reorder by up to p queue places (>0:earlier; <0:later; 0:random +/-5).\n");
        fprintf(stderr,"       Out-of-order delivery works best with some -l latency.\n");
        fprintf(stderr," -v    Verbose output of debug messages. More -vs may increase verbosity.\n");
        fprintf(stderr," -x p  Randomly duplicate about p%% of messages (including duplicates).\n");
        fprintf(stderr," -h    Print this help message.\n");
        exit(1);
      case '?':
        //if (optopt == 'c'){
        //  fprintf(stderr, "Option -%c requires an argument.\n", optopt
        //else
        if (isprint(optopt)){
          fprintf(stderr, "Unknown option `-%c'.\n", optopt);
        }else{
          fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
        }
        return 1;
      default:
        abort();
    }
  }
          
  if (argc - optind < 1){
    fprintf(stderr,"Usage: %s [options] port\n", argv[0]);
    exit(1);
  }
  port = atoi(argv[optind]);
  fprintf(stderr,"Unreliable Relay Server v06\n");
  fprintf(stderr,"now64:%lld\n",now64());
  if (flag_verbose > 1) fprintf(stderr,"now64:%lld\n",now64()/1000000);
  fprintf(stderr,"flag_verbose:%d flag_drop:%d\n",flag_verbose,flag_drop);
  fprintf(stderr,"flag_reorder_rate:%d flag_reorder_step:%d\n", flag_reorder_rate, flag_reorder_step);
  fprintf(stderr,"flag_corrupt_rate:%d flag_corrupt_type:%d\n", flag_corrupt_rate, flag_corrupt_type);
  fprintf(stderr,"flag_latency:%d flag_duplicate_rate:%d\n", flag_latency, flag_duplicate_rate);

  // set up server socket
  welcomesockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (welcomesockfd < 0){
    error("ERROR opening welcome socket");
  }
  memset((char *) &serv_addr,'\0',sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);
  if (bind(welcomesockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) 
    error("ERROR on bind()ing welcome socket");
  listen(welcomesockfd,5);
  fprintf(stderr, "listen()ing for client connections on server port %d\n", port);

  // accept() two client connections
  int i;
  for(i = 0; i < 2; i++){
    clilen = sizeof(cli_addr[i]);
    sessionsockfd[i] = accept(welcomesockfd, (struct sockaddr *) &cli_addr[i], &clilen);
    if (sessionsockfd[i] < 0){
      error("ERROR on accept");
    }
    fprintf(stderr, " client connection %d accept()ed\n", i);
  }

  // two sockets, therefore need to use select()/poll() to check for readiness so we don't block
  poll_array[0].fd = sessionsockfd[0];
  poll_array[0].events = POLLIN;
  poll_array[1].fd = sessionsockfd[1];
  poll_array[1].events = POLLIN;
  int loopcount = 1;
  memset(buffer[0],'\0',BUFSIZE);
  memset(buffer[1],'\0',BUFSIZE);
  buf_insert[0] = 0;
  buf_insert[1] = 0;

  /* create two message queues */
  msq[0] = make_queue();
  msq[1] = make_queue(); 

  /* initialise byte counters */
  client_bytes[0] = 0;
  client_bytes[1] = 0;

  /* zero client time stamps */
  client_start[0] = 0;
  client_start[1] = 0;
  client_latest[0] = 0;
  client_latest[1] = 0;

  /* loop forever, reading input from either socket and re-writing to the other */
  while(1){
    /* first-cut poll() implementation ...
       * Ignore the risk of output socket not being writeable ... for now, at least.
       * poll() must wait for a finite time only, because there may be queued message whose
         time-to-send has arrived. If we use a fixed timeout value, we have to trade off
         between lumpiness of latency time resolution and inefficiency tending towards
         busy-waiting.  But it is possible to ask each queue for the time its next message
         (if any) is due to be sent, and calculate the offset between now and the earliest
         send time to use as our poll() timeout.  The queues are sorted by send time, so
         this query is an O(1) operation.  If both queues are empty, we set the poll()
         timeout to INT_MAX milliseconds, which is longer than the program is going to be
         left running, and in that case poll() will block until input arrives. */
    int timeout = get_poll_timeout_milli(msq, 2);
    if (flag_verbose > 2) fprintf(stderr, "DEBUG: get_poll_timeout_milli(): %d\n", timeout);
    poll(poll_array, 2, timeout);

    /* Note that we need to process newline-terminated messages, but TCP does NOT
       preserve message boundaries, so we need to cater for multiple and/or partial
       messages (lines) per read().  Strategy is to append read() data into a
       persistent buffer, and then shuffle out any complete lines one-by-one,
       leaving any incomplete line in the front of the buffer to be appended to by
       the next read(). */
    int q = 0; // id of throughput queue matches input socket
    char arrow = '>';
    for(q = 0; q < 2; q++){
      arrow = q?'<':'>';
      if (poll_array[q].revents & POLLIN){
        /* append input from socket to buffer */
        buf_insert[q] = strlen(buffer[q]);
        n = read(sessionsockfd[q],buffer[q]+buf_insert[q],BUFSIZE-1-buf_insert[q]);
        if (n < 0) error("ERROR reading from sessionsockfd[q]");
        if (n == 0) error("Reached EOF on socket. Assume socket was abandoned by other end.");
        /* add to count of client bytes received - used for calculating protocol "efficiency" */
        client_bytes[q] += n;
        fprintf(OUT, "client_bytes[0]:%d client_bytes[1]:%d total:%d\n",
                client_bytes[0], client_bytes[1], client_bytes[0] + client_bytes[1]);
        /* update and report client timers */
        long long now = now64();
        if (!client_start[q]){
          client_start[q] = now;
          fprintf(OUT, "client %d timer initialised: %lld\n", q, client_start[q]);
        }
        client_latest[q] = now;
        fprintf(OUT, "client 0 elapsed us: %lld   client 1 elapsed us: %lld\n", 
                (client_latest[0] - client_start[0]) / 1000, (client_latest[1] - client_start[1]) / 1000);
        /* identify and individually process any/all newline-terminated messages */
        while(enqueue_message(q)){}
      }
    }
    if (flag_verbose > 1){
      dump_queue(msq[0]);
      dump_queue(msq[1]);
    }
    /* send as many queued messages as we can, alternating between queues until both are empty */
    int sent[2];
    do{
      sent[0] = send_message(0);
      sent[1] = send_message(1);
    }while (sent[0] || sent[1]);
  }

  // do we even ever get here?
  fprintf(stderr, "closing sockets\n");
  close(sessionsockfd[0]);
  close(sessionsockfd[1]);
  close(welcomesockfd);
  return 0; 
}

// Check buffered input from input specified socket.  If a newline-terminated message
// is present, 'process' it (i.e. place it in the opposite-numbered send queue for
// later output on the corresponding socket), and slide remaining buffer content
// forwards to remove the processed message from the read buffer.
int enqueue_message(int channel){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting enqueue_message()\n");
  #endif
  int j = 0;
  char c = 0;
  char *msg = 0;
  char arrow = '>';
  arrow = channel?'<':'>';

  switch (flag_verbose){
    case 0:
      break;
    case 1:
      dumpbuf(buffer[channel],BUFSIZE);
      break;
    default:
      dumpbuf(buffer[0],BUFSIZE);
      dumpbuf(buffer[1],BUFSIZE);
  }
  while(buffer[channel][j] && buffer[channel][j] != '\n' && j < BUFSIZE){
    j++;
  }
  // j is now the index of a newline or a null or the end of the buffer ... but which?
  if (j >= BUFSIZE){
    fprintf(stderr, "Input message too long for buffer. Aborting.\n");
    exit(0);
  }
  if (buffer[channel][j] == '\n'){
    // Houston, we have a newline-terminated message!
    j++; // increment j to include newline in message string
    msg = malloc(j+1);
    memset(msg,'\0',j+1);
    strncpy(msg,buffer[channel],j);
    memmove(buffer[channel], buffer[channel]+j, BUFSIZE-j);
    memset(buffer[channel]+BUFSIZE-j,'\0',j);
    /* randomly choose whether to forward this message or not */
    int inverseDropRate = 0;
    switch (flag_drop){
      case 1:
        inverseDropRate = 10;
        break;
      case 2:
        inverseDropRate = 4;
        break;
      case 3:
        inverseDropRate = 2;
        break;
    }
    /* If inverseDropRate is zero, don't divide by zero(!), and don't drop messages. */
    if(inverseDropRate?rand()%inverseDropRate:1){
      /* randomly choose whether to corrupt this message or not */
      randomly_corrupt(msg);
      /* if reordering is chosen, set additional delay on about 20% of messages */
      /* place this message into opposite-numbered send queue for later writing to socket */
      enqueue(msq[1-channel], msg, flag_latency);
      if (rand()%100 < flag_reorder_rate){
        #ifdef DEBUG
          fprintf(stderr, "DEBUG: enqueue_message(): 1.3\n");
        #endif
        reorder(msq[1-channel], flag_reorder_step);
        #ifdef DEBUG
          fprintf(stderr, "DEBUG: enqueue_message(): 1.4\n");
        #endif
        fprintf(OUT,"#reordered#");
      }
      fprintf(OUT,"#forwarded# %c %s", arrow, msg);
      /* randomly add duplicates, including possibly duplicates of duplicates */
      int duplicate_count = 1;
      while (rand()%100 < flag_duplicate_rate){
        enqueue(msq[1-channel], strdup(msg), flag_latency + duplicate_count++);
        fprintf(OUT,"#duplicate# %c %s", arrow, msg);
      }
    }else{
      fprintf(OUT,"#dropped# %c %s", arrow, msg);
    }
    return 1;
  }else{
    #ifdef DEBUG
      fprintf(stderr,"---no newline found in buffer[%d]:%s:\n",channel,buffer[channel]);
    #endif
    return 0;
  }
}

/* take one message from specified send queue, and write() it into the corresponding socket */
int send_message(int sq){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting send_message()\n");
  #endif
  char *msg = 0;
  if (msg = dequeue(msq[sq])){
    /* write to socket (at last!) */
    /* NOTE: this may block if the socket's buffer is full. But that'll only happen
             temporarily, or if there's a problem in the way the client operates. */
    int n = write(sessionsockfd[sq],msg,strlen(msg));
    if (n < 0) error("ERROR writing to sessionsockfd[sq]");
    free(msg);
    return 1;
  }else{
    /* nothing to send from this queue at this time */
    return 0;
  }
}


/* randomly choose whether to corrupt this message.
   Corruption can include changing characters and truncating the string, with or without
   adding a newline before the null-terminator.  Note that under our line-based protocol,
   inserting a newline makes the message into two messages, and will (likely) test the 
   client's ability to separate multiple messages obtained in a single read() from its
   end of the socket. */
void randomly_corrupt(char *msg){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting randomly_corrupt()\n");
  #endif
  /* firstly, decide *whether* to corrupt this message or not */
  if (rand()%100 >= flag_corrupt_rate){
    /* leave this message intact */
    return;
  }
  /* ok, we decided to corrupt, so display the uncorrupted message */
  fprintf(stderr, "#corrupting# ");
  int length = 0;
  length = strlen(msg);
  dumpbuf(msg, length);
  if (length < 2){
    /* do nothing, because message is an empty line */
    if (flag_verbose > 0) fprintf(stderr, "randomly_corrupt() doing nothing: string is too short\n");
    return;
  }
  /* next decision is what type of corruption ... */
  switch (flag_corrupt_type){
    case 1:
      corrupt_character_flip(msg);
      break;
    case 2:
      corrupt_insert_newline(msg);
      break;
    case 3:
      corrupt_truncate_clean(msg);
      break;
    case 4:
      corrupt_truncate_dirty(msg);
      break;
    default:
      fprintf(stderr, "ERROR: no such corruption type implemented (yet): %d\n", flag_corrupt_type);
  }
  /* display the corrupted message */
  fprintf(stderr, "#corrupted#  ");
  dumpbuf(msg, length);
}

/* change a character OTHER than the terminating newline, and not to NULL or newline */
void corrupt_character_flip(char *msg){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting corrupt_character_flip():\n");
  #endif
  int x = 0;
  int i = 0;
  if (strlen(msg) < 2){
    /* do nothing, because message is an empty line and we are not messing with newlines here */
    if (flag_verbose > 0) fprintf(stderr, "corrupt_character_flip() doing nothing: string is too short\n");
  }else{
    /* above check should protect us from dividing by zero below */
    /* munge up to 3 characters. Note: random() may land on the same character more than once. */
    for (i = 1; i <=3; i++){
      x = random() % (strlen(msg)-1);
      if (msg[x] != 'X'){
        msg[x] = 'X';
      }else{
        msg[x] = '.';
      }
    }
  }
}

/* break message in two by changing a random character to a newline */
void corrupt_insert_newline(char *msg){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting corrupt_insert_newline()\n");
  #endif
  int x = 0;
  if (strlen(msg) < 2){
    /* do nothing, because message is an empty line and we can't insert another newline here */
    if (flag_verbose > 0) fprintf(stderr, "corrupt_insert_newline() doing nothing: string is too short\n");
  }else{
    /* above check should protect us from dividing by zero */
    x = random() % (strlen(msg)-1);
    msg[x] = '\n';
  }
}

/* do a 'clean' truncation: insert a newline followed by a null */
void corrupt_truncate_clean(char *msg){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting corrupt_truncate_clean()\n");
  #endif
  int x = 0;
  if (strlen(msg) < 2){
    /* do nothing, because message is an empty line and we can't shorten it */
    if (flag_verbose > 0) fprintf(stderr, "corrupt_truncate_clean() doing nothing: string is too short\n");
  }else{
    /* above check should protect us from dividing by zero */
    x = random() % (strlen(msg)-1);
    msg[x] = '\n';
    msg[x+1] = '\0';
  }
}

/* do a 'dirty' truncation: insert a null WITHOUT a preceding newline 
   This turns a message into a part-message and may cause confusion for an unsophisticated client.
   It also risks making a message that is longer than the specified max length, so should be used
   with "short" messages and moderately low corruption rates. */
void corrupt_truncate_dirty(char *msg){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: starting corrupt_truncate_dirty()\n");
  #endif
  int x = 0;
  if (strlen(msg) < 2){
    /* do nothing, because message is an empty line and we can't insert another newline here */
    if (flag_verbose > 0) fprintf(stderr, "corrupt_insert_newline() doing nothing: string is too short\n");
  }else{
    /* above check should protect us from dividing by zero */
    x = random() % (strlen(msg)-1);
    msg[x] = '\0';
  }
}

