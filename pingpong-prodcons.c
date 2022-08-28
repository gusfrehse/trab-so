#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ppos.h"
#include "ppos_data.h"

#define ITERATIONS 200
#define SLEEP_TIME 200

#define NUM_PROD 10
#define NUM_CONS 4

#define BUF_SIZE 12

struct cbuf {
  char buf[BUF_SIZE];
  int pos_write;
  int pos_read;
} cbuf = {};

semaphore_t s_vaga;
semaphore_t s_item;
semaphore_t s_buffer;

void insert(int n) {
  cbuf.buf[cbuf.pos_write] = n;

  cbuf.pos_write = (cbuf.pos_write + 1) % BUF_SIZE;
}

int take() {
  int val = cbuf.buf[cbuf.pos_read];

  cbuf.pos_read = (cbuf.pos_read + 1) % BUF_SIZE;

  return val;
}

void produtor() {
  srandom(time(NULL));
  int i = 0;
  while (i < ITERATIONS) {
    task_sleep(SLEEP_TIME);

    int item = rand() % 100;

    sem_up(&s_item);


    sem_down(&s_vaga);



    sem_down(&s_buffer);


    insert(item);

    sem_up(&s_buffer);


    printf("task id %d prod: produziu %d\n", task_id(), item);
  }

  task_exit(0);
}

void consumidor() {
  int i = 0;
  while (i < ITERATIONS) {
    task_sleep(SLEEP_TIME);
    int item;

    sem_down(&s_item);

    sem_down(&s_buffer);

    item = take();


    sem_up(&s_buffer);

    sem_up(&s_vaga);
    printf("task id %d cons:                   consumiu %d\n", task_id(), item);
  }

  task_exit(0);
}

int main() {
  task_t produtores[NUM_PROD];
  task_t consumidores[NUM_CONS];

  ppos_init();

  sem_create(&s_vaga, BUF_SIZE);
  sem_create(&s_item, 0);
  sem_create(&s_buffer, 1);

  for (int i = 0; i < NUM_PROD; i++) {
    task_create(produtores + i, produtor, 0);
  }

  for (int i = 0; i < NUM_CONS; i++) {
    task_create(consumidores + i, consumidor, 0);
  }

  for (int i = 0; i < NUM_PROD; i++) {
    task_join(produtores + i);
  }

  for (int i = 0; i < NUM_CONS; i++) {
    task_join(consumidores + i);
  }

  task_exit(0);

  return 0;
}
