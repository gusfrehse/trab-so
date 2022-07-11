// GRR20203927 Gustavo Silveira Frehse

#include "ppos.h"
#include "ppos_data.h"
#include "queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

//#define DEBUG

#define STACKSIZE 1024 * 64
#define SCHED_PRIO_ALPHA -1

enum task_status {
  TASK_READY = 0x0,
  TASK_TERMINATED = 0x1,
  TASK_SUSPENDED = 0x2,
};

int next_id = 0;
task_t main_task;
task_t dispatcher_task;
task_t *current_task;

queue_t *task_queue = NULL;
int user_tasks = 0;

void *print_task_queue_aux(queue_t *q) {
  task_t* t = (task_t *)q;
  printf("%3d:%-4lld", t->id, t->prio_din);

  return NULL;
}

task_t *scheduler() {
#ifdef DEBUG
  queue_print("[i] DEBUG scheduler task queue", (queue_t *)task_queue,
              (void (*)(void *))print_task_queue_aux);
#endif

  task_t *curr = (task_t*)task_queue;
  curr->prio_din += SCHED_PRIO_ALPHA;
  task_t* chosen = curr;

  while((curr = curr->prev) != (task_t*) task_queue) {
    curr->prio_din += SCHED_PRIO_ALPHA;
    if (chosen->prio_din > curr->prio_din) {
      chosen = curr;
    }
  }


  chosen->prio_din = chosen->prio_est;

  return (task_t *)chosen;
}

void dispatcher(void *nothing) {
#ifdef DEBUG
  printf("[i] DEBUG scheduler started\n");
#endif

  while (user_tasks > 0) {
    task_t *chosen_task = scheduler();
    if (chosen_task) {

#ifdef DEBUG
      printf("[i] DEBUG scheduler found task id %d\n", chosen_task->id);
#endif

      // Execute task.
      task_switch(chosen_task);
      // Task yielded/exitted.

      switch (chosen_task->status) {
      case TASK_READY:
        task_queue = task_queue->next;
        break;
      case TASK_TERMINATED:
        queue_remove((queue_t **)&task_queue, (queue_t *)chosen_task);
        user_tasks--;
        free(chosen_task->context.uc_stack.ss_sp);
        break;
      case TASK_SUSPENDED:
        // ??? TODO
        break;
      }
    }
#ifdef DEBUG
    else {
      printf("[i] DEBUG scheduler found no new task\n");
    }
#endif
  }

  task_switch(&main_task);
}

void ppos_init() {
  // Initialize main task.
  getcontext(&(main_task.context));
  main_task.next = main_task.prev = NULL;
  main_task.id = next_id++;

  current_task = &main_task;

  // printf magic
  setvbuf(stdout, 0, _IONBF, 0);

  // creating dispatcher task
  task_create(&dispatcher_task, dispatcher, NULL);
  queue_remove((queue_t **)&task_queue, (queue_t *)&dispatcher_task);
  user_tasks = 0; // reset because dispatcher is not a 'user task'.

#ifdef DEBUG
  printf("[i] DEBUG initialized ppos\n");
#endif

  return;
}

int task_create(task_t *task, void (*start_func)(void *), void *arg) {
  getcontext(&(task->context));

  char *stack = (char *)malloc(STACKSIZE);
  if (!stack) {
#ifdef DEBUG
    printf("[-] ERROR allocating memory for task's stack\n");
#endif
    return -1;
  }

  task->context.uc_stack.ss_sp = stack;
  task->context.uc_stack.ss_size = STACKSIZE;
  task->context.uc_stack.ss_flags = 0;
  task->context.uc_link = 0;

  makecontext(&(task->context), (void (*)(void))start_func, 1, (void *)arg);

  task->next = task->prev = NULL;
  task->id = next_id++;
  task->status = TASK_READY;

#ifdef DEBUG
  printf("[i] DEBUG created thread id: %d\n", task->id);
#endif

  queue_append((queue_t **)&task_queue, (queue_t *)task);
  user_tasks++;

  return task->id;
}

int task_switch(task_t *task) {
#ifdef DEBUG
  printf("[i] DEBUG switching tasks from id %d to id %d\n", current_task->id,
         task->id);
#endif

  task_t *old_task = current_task;
  current_task = task;
  swapcontext(&old_task->context, &current_task->context);

  return 0;
}

void task_exit(int exit_code) {
#ifdef DEBUG
  printf("[i] DEBUG exiting task id %d with exit code %d\n", current_task->id,
         exit_code);
#endif

  task_t *old_task = current_task;
  current_task = &dispatcher_task;

  old_task->status = TASK_TERMINATED;
  swapcontext(&old_task->context, &current_task->context);
}

int task_id() { return current_task->id; }

void task_yield() {
#ifdef DEBUG
  printf("[i] DEBUG task id %d yielded\n", current_task->id);
#endif
  current_task->status = TASK_READY;
  task_switch(&dispatcher_task);
}

void task_setprio(task_t *task, int prio) {
  if (!task) {
    task = current_task;
  }

#ifdef DEBUG
  printf("[i] DEBUG changing prio task id %d from %lld to %d\n", task->id, task->prio_est, prio);
#endif

  task->prio_est = prio;
  task->prio_din = prio;
}

int task_getprio(task_t *task) {
  if (!task)
    task = current_task;

  return task->prio_est;
}

