/* Set of utility functions for relay-server.c and client.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <assert.h>
#include <limits.h>
#include "urs-util.h"

//#define DEBUG

/* Print out contents of a buffer (plus the byte *after* the buffer),
   translating chars such as '\n' and '\0' to a more readable form. 
   Useful in debugging buffer manipulation code. */
void dumpbuf(char* buf, int size){
  int i = 0;
  fprintf(stderr,"dumpbuf():%d:",size);
  for(i = 0; i < size+1; i++){
    if (i == size) fputc(':',stderr); // mark end of buffer
    switch(buf[i]){
      case '\n':
        fputc('N',stderr);
        break;
      case '\0':
        fputc('0',stderr);
        break;
      case '\t':
        fputc('T',stderr);
        break;
      case '\r':
        fputc('R',stderr);
        break;
      default:
        fputc(buf[i],stderr);
    }
  }
  fputc('\n',stderr);
}

/* Report a system call error condition and exit. */
void error(const char *msg){
  perror(msg);
  exit(1);
}

/* interface functions for send queue */
/* create a new message queue */
struct mq *make_queue(){
  struct mq *q = (struct mq*)malloc(sizeof(struct mq));
  if (!q) error("ERROR: malloc() failed in make_queue()\n");
  bzero(q, sizeof(struct mq));
  return q;
}

/* insert message into queue sorted by time_gate */
void enqueue(struct mq *q, char *msg, int delay_ms){
  struct mqn *m = (struct mqn*)malloc(sizeof(struct mqn));
  if (!m) error("ERROR: malloc() failed in function enqueue()\n");
  bzero(m, sizeof(struct mqn));
  m->msg = msg;
  m->next = 0;
  /* calculate time_gate in microseconds as current time plus delay milliseconds */
  m->time_gate = now64() + delay_ms*1000;

  if (!q->tail){
    /* insert into empty queue */
    assert(!q->head);
    q->head = m;
    q->tail = m;
  }else{
    assert(q->tail);
    assert(q->head);
    /* insert into queue sorted by time_gate value (smallest first) */
    if (m->time_gate >= q->tail->time_gate){
      /* append after tail of queue - should be the common case */
      q->tail->next = m;
      m->prev = q->tail;
      q->tail = m;
    }else if (m->time_gate < q->head->time_gate){
      /* insert before head of queue */
      m->next = q->head;
      q->head->prev = m;
      q->head = m;
    }else{
      /* walk the list to find insert point */
      /* TODO: could start at tail, now that list is doubly-linked ... ?? */
      struct mqn *insert, *previous;
      assert(q->head != q->tail); // at least 2 items in queue
      insert = q->head;
      while (insert->time_gate <= m->time_gate){
        previous = insert;
        insert = insert->next;
      }
      /* insert here, between 'previous' and 'insert' */
      assert(insert && previous);
      m->next = insert;
      insert->prev = m;
      previous->next = m;
      m->prev = previous;
    }
  }
  #ifdef DEBUG
    dump_queue(q);
  #endif
}

/* Retrieve and remove the message at the head of the queue - but only if it's
   time_gate is less than the current system time. Queue is sorted by time_gate,
   so if the head item is not ready to go yet, we don't have to bother checking
   any others :-). */
char *dequeue(struct mq *q){
  if (!q->head){
    /* queue is empty */
    assert(!q->tail);
    return 0;
  }else{
    /* queue is non-empty */
    assert(q->tail);
    long long now = now64();
    #ifdef DEBUG
      fprintf(stderr, "DEBUG dequeue(): now64():%lld time_gate:%lld remain:%lld\n",
              now, q->head->time_gate, q->head->time_gate - now);
    #endif
    if (now < q->head->time_gate){
      /* it's still too early to send this message */
      return 0;
    }
    /* the head message is ready to send, so let's remove it from the queue */
    struct mqn *m = q->head;
    q->head = q->head->next;
    if (q->head){
      q->head->prev = 0;
    }else{
      /* we just removed last item, so queue is now empty */
      q->tail = 0;
    }
    char *msg = m->msg;
    free(m);
    #ifdef DEBUG
      dump_queue(q);
    #endif
    return msg;
  }
}

/* reorder a message near the tail end of the queue.
   If the queue contains enough items to do so, a POSITIVE step will move the tail item
   forward step places in the queue, and a NEGATIVE step will move the stepth-from-last
   item to the tail of the queue.  A ZERO step does nothing at all.
   When a message is moved, it's time_gate value will be changed to suit its new location
   by setting it equal to the time_gate of the item it is being placed before/behind.
   If there are not enough items in the queue to make the requested move, then .... ???
   Reordering will obviously only work meaningfully if there are enough items in the
   queue.  The easy way to ensure this is to set a suitable latency value (with -l) so
   that messages spend enough time in the queue to potentially be reordered.
   By default, new messages are added to the tail end of the queue, so it seems best to
   do reordering there rather than at the head of the queue.  If we did it at the head,
   we might reorder a small number of messages many times each, but reordering a larger
   number of messages a small (one?) number of times each sounds more like what we want. */
void reorder(struct mq *q, int step){
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: reorder(): 1.0\n");
  #endif
  if (q->head == q->tail){
    /* queue is either empty or has only one item, so can't reorder anything */
    return;
  }
  while (!step){
    /* step is zero, so randomise it in the range -5..+5 */
    step = rand()%11 - 5;
  }
  
  /* find the item which is |step| items back from the tail of the list */
  int steps = step<0?0-step:step;
  struct mqn *p = q->tail;
  while (p && p->prev && steps){
    p = p->prev;
    steps--;
  }
  #ifdef DEBUG
    fprintf(stderr, "DEBUG: reorder(): 2.0\n");
  #endif
  /* now p is |step| positions back from the tail of the list, or is at the
     head of the list if the list did not contain enough items */
  if (step < 0){
    #ifdef DEBUG
      fprintf(stderr, "DEBUG: reorder(): 2.1\n");
    #endif
    /* move the item at p down to the tail of the queue */
    if (p == q->head){
      #ifdef DEBUG
        fprintf(stderr, "DEBUG: reorder(): 2.1.1\n");
      #endif
      /* disconnect head node */
      assert(q->head);
      q->head = q->head->next;
      q->head->prev = 0;
      #ifdef DEBUG
        fprintf(stderr, "DEBUG: reorder(): 2.1.2\n");
      #endif
    }else{
      #ifdef DEBUG
        fprintf(stderr, "DEBUG: reorder(): 2.1.3a\n");
      #endif
      /* disconnect non-head node */
      assert(p);
      assert(p->prev);
      assert(p->next);
      p->prev->next = p->next;
      #ifdef DEBUG
        fprintf(stderr, "DEBUG: reorder(): 2.1.3b\n");
      #endif
      p->next->prev = p->prev;
      #ifdef DEBUG
        fprintf(stderr, "DEBUG: reorder(): 2.1.4\n");
      #endif
    }
    /* reconnect this node at tail of queue */
    q->tail->next = p;
    p->prev = q->tail;
    p->next = 0;
    q->tail = p;
    #ifdef DEBUG
      fprintf(stderr, "DEBUG: reorder(): 2.1.5\n");
    #endif
    /* update time_gate for moved node to keep queue time-sorted */
    q->tail->time_gate = q->tail->prev->time_gate;
    #ifdef DEBUG
      fprintf(stderr, "DEBUG: reorder(): 2.2\n");
    #endif

  }else{
    #ifdef DEBUG
      fprintf(stderr, "DEBUG: reorder(): 2.3\n");
    #endif
    assert(step > 0);
    /* move the tail of the queue up in front of the item at p */
    /* disconnect from tail */
    struct mqn *t = q->tail;
    q->tail = q->tail->prev;
    q->tail->next = 0;
    if (p == q->head){
      /* insert before p as new head of queue */
      t->next = p;
      t->prev = 0;
      p->prev = t;
      q->head = t;
    }else{
      /* insert before p as non-head node */
      t->next = p;
      t->prev = p->prev;
      p->prev->next = t;
      p->prev = t;
    }
    #ifdef DEBUG
      fprintf(stderr, "DEBUG: reorder(): 2.4\n");
    #endif
  }

}

/* print queue contents to stderr for debugging */
void dump_queue(struct mq *q){
  fprintf(stderr, "  --- dump_queue():\n");
  struct mqn *n = q->head;
  long long now = now64();
  while(n){
    fprintf(stderr, "  time_gate:%lld remain:%lld ", n->time_gate, now - n->time_gate); 
    dumpbuf(n->msg, strlen(n->msg));
    n = n->next;
  }
  fprintf(stderr, "  --- end of dump_queue() ---------\n");
}

/* get time in microseconds when next message is due to be send from a queue */
long long get_next_send_time_micro(struct mq *q){
  if (!q->head){
    /* queue is empty, so send_time is approximately "never" */
    return LLONG_MAX;
  }else{
    /* head of queue will have earliest time_gate */
    assert(q->head);
    return q->head->time_gate;
  }
}

/* get min poll() timeout for set of queues
   Intention:
    If *any* queue has a past-due message, poll() timeout should be zero so
    that poll() does not block at all.
    If *both* queues are empty, poll() can block with a Very Long timeout.
    Otherwise timeout should be lowest positive interval. */
int get_poll_timeout_milli(struct mq **queues, int q_count){
  /* default to blocking for a Very Long time */
  long long timeout = LLONG_MAX;
  /* take a timestamp so our comparison point is not moving */
  long long now = now64();
  /* choose smallest remain time from all queues */
  int loop = 0;
  for(loop = 0; loop < q_count; loop++){
    long long remain = get_next_send_time_micro(queues[loop]) - now;
    if (remain < timeout){
      timeout = remain;
    }
  }
  /* negative remain time => past-due message, so poll() timeout should be zero
     to instruct poll() not to block at all */
  if (timeout < 0){
    timeout = 0;
  }else{
    /* convert from microseconds to milliseconds and cap at INT_MAX */
    timeout = timeout / 1000;
    if (timeout > INT_MAX){
      timeout = INT_MAX;
    }
  }
  assert(timeout <= INT_MAX);
  assert(timeout >= 0);
  return (int)timeout;
}

/* get current system time as a 64-bit integer in microseconds */
long long now64(){
  struct timeval now;
  bzero(&now, sizeof(struct timeval));
  if (gettimeofday(&now,0)) error("ERROR: gettimeofday() failed\n");
  return  (long long)(now.tv_sec)*1000000 + now.tv_usec;
}

