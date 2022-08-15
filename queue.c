// GRR20203927 Gustavo Silveira Frehse

#include "queue.h"

#include <stdio.h>
#include <stdlib.h>

int queue_size(queue_t *queue) {
  if (queue == NULL)
    return 0;

  int size = 1;
  queue_t *curr = queue->next;
  while (curr != queue) {
    size++;
    curr = curr->next;
  }

  return size;
}

void queue_print(char *name, queue_t *queue, void print_elem(void *)) {
  printf("%s: [", name);
  if (queue) {
    print_elem((void *)queue);

    queue_t *curr = queue->next;
    while (curr != queue) {
      putchar(' ');
      print_elem((void *)curr);
      curr = curr->next;
    }
  }

  printf("]\n");
}

int queue_append(queue_t **queue, queue_t *elem) {
  if (!queue) {
    fprintf(stderr, "[-] ERROR  in '%s': queue does not exist.\n",
            __FUNCTION__);
    exit(1);
    return 1;
  }

  if (!elem) {
    fprintf(stderr, "[-] ERROR  in '%s': elem does not exist.\n", __FUNCTION__);
    exit(2);
    return 2;
  }

  if (elem->next || elem->prev) {
    fprintf(stderr, "[-] ERROR  in '%s': elem is already in a queue.\n",
            __FUNCTION__);
    exit(3);
    return 3;
  }

  // Check if queue is empty.
  if (!*queue) {
    *queue = elem;
    elem->next = elem;
    elem->prev = elem;
    return 0;
  }

  queue_t *last = (*queue)->prev;
  (*queue)->prev = elem;
  elem->next = *queue;

  last->next = elem;
  elem->prev = last;

  return 0;
}

int queue_remove(queue_t **queue, queue_t *elem) {
  if (!queue) {
    fprintf(stderr, "[-] ERROR  in '%s': queue does not exist.\n",
            __FUNCTION__);
    exit(1);
    return 1;
  }

  if (!elem) {
    fprintf(stderr, "[-] ERROR  in '%s': elem does not exist.\n", __FUNCTION__);
    exit(2);
    return 2;
  }

  if (!*queue) {
    fprintf(stderr, "[-] ERROR  in '%s': queue is empty.\n", __FUNCTION__);
    exit(3);
    return 3;
  }

  // search queue for elem
  queue_t *curr = (*queue)->next;
  while (curr != elem) {
    if (curr == *queue) {
      fprintf(stderr, "[-] ERROR  in '%s': elem is not in queue.\n",
              __FUNCTION__);
      exit(4);
      return 4;
    }
    curr = curr->next;
  }

  // Only one element
  if (elem == elem->next) {
    *queue = NULL;
    elem->prev = NULL;
    elem->next = NULL;
    return 0;
  }

  if (elem == *queue) {
    *queue = elem->next;
  }

  elem->prev->next = elem->next;
  elem->next->prev = elem->prev;

  elem->prev = NULL;
  elem->next = NULL;
  return 0;
}
