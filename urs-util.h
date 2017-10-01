/* Set of utility functions for (unreliable) relay-server.c and client.c */

/* Print out contents of a buffer (plus the byte *after* the buffer),
   translating chars such as '\n' and '\0' to a more readable form. 
   Useful in debugging buffer manipulation code. */
void dumpbuf(char* buf, int size);

/* get current system time in microseconds since epoch as a 64-bit number */
long long now64();

/* Report a system call error condition and exit. */
void error(const char *msg);

/* data structures and interface for send queue (we need two of these) */
/* message queue node */
struct mqn{
  char *msg;
  long long time_gate; // don't send before
  struct mqn *next;
  struct mqn *prev;
};
/* message queue - pointers to a linked list of nodes */
struct mq{
  struct mqn *head;
  struct mqn *tail;
};
/* message queue manipulation functions (interface) */
struct mq *make_queue();
void enqueue(struct mq *q, char *msg, int delay_ms);
char *dequeue(struct mq *q);
void reorder(struct mq *q, int step);
void dump_queue(struct mq *q);

int get_poll_timeout_milli(struct mq **queues, int q_count);
