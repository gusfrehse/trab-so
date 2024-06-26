// GRR20203927 Gustavo Silveira Frehse

#include "ppos.h"
#include "ppos_data.h"
#include "queue.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <ucontext.h>
#include <string.h>

//#define DEBUG

#define STACKSIZE 1024 * 64
#define SCHED_PRIO_ALPHA -1
#define SCHED_QUANTUM 20

enum task_status {
  TASK_READY = 0x0,
  TASK_TERMINATED = 0x1,
  TASK_SUSPENDED = 0x2,
};

int next_id = 0;
task_t main_task;
task_t dispatcher_task;
task_t *current_task;

task_t *task_queue = NULL;
int user_tasks = 0;

task_t *sleep_queue = NULL;

unsigned int current_time = 0;

int preemption_lock = 0;

struct sigaction preemption_action;
struct itimerval preemption_timer = {0};

task_t *scheduler();
void dispatcher();
void tick(int signum);
static void print_stats();

void *print_task_queue_aux(queue_t *q) {
  task_t *t = (task_t *)q;
  printf("%2d ", t->id);

  return NULL;
}

void ppos_init() {
  // Initialize main task.
  getcontext(&(main_task.context));
  main_task.next = main_task.prev = NULL;
  main_task.id = next_id++;
  main_task.system_task = 0;
  main_task.preemptable = 1;
  main_task.cpu_time = 0;
  main_task.start_time = systime();
  main_task.activations = 0;
  main_task.join_queue = NULL;
  main_task.join_return_code = 0;
  main_task.wake_up_time = 0;
  queue_append((queue_t **)&task_queue, (queue_t *)&main_task);
  user_tasks++;

  current_task = &main_task;

  // printf magic
  setvbuf(stdout, 0, _IONBF, 0);

  // creating dispatcher task
  task_create(&dispatcher_task, dispatcher, NULL);
  queue_remove((queue_t **)&task_queue, (queue_t *)&dispatcher_task);
  user_tasks--; // reset because dispatcher is not a 'user task'.
  dispatcher_task.system_task = 1;
  dispatcher_task.preemptable = 0;
  dispatcher_task.cpu_time = 0;
  dispatcher_task.start_time = systime();
  dispatcher_task.activations = 0;
  dispatcher_task.join_queue = NULL;
  dispatcher_task.join_return_code = 0;
  dispatcher_task.wake_up_time = 0;

  // create signal handler for task preemption
  preemption_action.sa_handler = tick;
  sigemptyset(&preemption_action.sa_mask);
  preemption_action.sa_flags = 0;

  if (sigaction(SIGALRM, &preemption_action, 0) < 0) {
#ifdef DEBUG
    perror("sigaction: ");
    printf("[-] ERROR setting up preemption action\n");
#endif
    exit(1);
  }

  // activate timer for preemption
  preemption_timer.it_value.tv_usec = 1000;
  preemption_timer.it_value.tv_sec = 0;
  preemption_timer.it_interval.tv_usec = 1000;
  preemption_timer.it_interval.tv_sec = 0;
  if (setitimer(ITIMER_REAL, &preemption_timer, 0) < 0) {
#ifdef DEBUG
    perror("setitimer: ");
    printf("[-] ERROR setting up preemption timer\n");
#endif
    exit(1);
  }

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
    return 1;
  }

  task->context.uc_stack.ss_sp = stack;
  task->context.uc_stack.ss_size = STACKSIZE;
  task->context.uc_stack.ss_flags = 0;
  task->context.uc_link = 0;

  makecontext(&(task->context), (void (*)(void))start_func, 1, (void *)arg);

  task->next = task->prev = NULL;
  task->id = next_id++;
  task->status = TASK_READY;
  task->preemptable = 1;
  task->system_task = 0;
  task->cpu_time = 0;
  task->start_time = systime();
  task->activations = 0;
  task->join_queue = NULL;
  task->join_return_code = 0;
  task->wake_up_time = 0;

#ifdef DEBUG
  printf("[i] DEBUG created thread id: %d\n", task->id);
#endif

  queue_append((queue_t **)&task_queue, (queue_t *)task);
  user_tasks++;

  return task->id;
}

int task_switch(task_t *task) {
#ifdef DEBUG
  printf("[i] DEBUG switching tasks from id %d to id %d with status %d\n",
         current_task->id, task->id, task->status);
#endif

  task->activations++;

  task_t *old_task = current_task;
  current_task = task;

#ifdef DEBUG
  printf("[i] DEBUG will switch!! tasks from id %d to id %d with status %d\n",
         old_task->id, task->id, task->status);

  printf("[i] DEBUG:   current task id %d status %d preemptable %d quantum %d "
         "system_task %d prio_est %d prio_din %d\n",
         old_task->id, old_task->status, old_task->preemptable,
         old_task->quantum, old_task->system_task, old_task->prio_est,
         old_task->prio_din);
  printf("[i] DEBUG:   next task id %d status %d preemptable %d quantum %d "
         "system_task %d prio_est %d prio_din %d\n",
         current_task->id, current_task->status, current_task->preemptable,
         current_task->quantum, current_task->system_task,
         current_task->prio_est, current_task->prio_din);

#endif
  swapcontext(&old_task->context, &current_task->context);

  return 0;
}

void task_exit(int exit_code) {
#ifdef DEBUG
  printf("[i] DEBUG exiting task id %d with exit code %d\n", current_task->id,
         exit_code);
#endif

  print_stats();

  // libera todas as tasks esperando a current
  while (queue_size((queue_t *)current_task->join_queue) > 0) {
    task_t *curr = current_task->join_queue;
    curr->join_return_code = exit_code;
    task_resume(curr, &current_task->join_queue);
  }

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
  task_switch(&dispatcher_task);
}

void task_setprio(task_t *task, int prio) {
  if (!task) {
    task = current_task;
  }

#ifdef DEBUG
  printf("[i] DEBUG changing prio task id %d from %lld to %d\n", task->id,
         task->prio_est, prio);
#endif

  task->prio_est = prio;
  task->prio_din = prio;
}

int task_getprio(task_t *task) {
  if (!task)
    task = current_task;

  return task->prio_est;
}

task_t *scheduler() {
#ifdef DEBUG
  // queue_print("[i] DEBUG scheduler task queue", (queue_t *)task_queue,
  //             (void (*)(void *))print_task_queue_aux);
#endif

  if (!task_queue)
    return NULL;

  task_t *curr = (task_t *)task_queue;
  curr->prio_din += SCHED_PRIO_ALPHA;
  task_t *chosen = curr;

  // Search for the highest priority task while 'aging' the other tasks.
  while ((curr = curr->prev) != (task_t *)task_queue) {
    curr->prio_din += SCHED_PRIO_ALPHA;
    if (chosen->prio_din > curr->prio_din) {
      chosen = curr;
    }
  }

  chosen->prio_din = chosen->prio_est;

  return (task_t *)chosen;
}

static void wake_up_tasks() {
  task_t *curr = sleep_queue;
  task_t *next;

  if (!curr)
    return;

  do {
    next = curr->next;

    // checa se curr precisa acordar
    if (curr->wake_up_time <= systime()) {
      task_resume(curr, &sleep_queue);
    }
  } while ((curr = next) != sleep_queue && sleep_queue);
}

void dispatcher() {
#ifdef DEBUG
  printf("[i] DEBUG scheduler started\n");
#endif
  unsigned int dispatcher_start_time = systime();
  unsigned int dispatcher_acc_negative_time = 0;

  while (user_tasks > 0) {

    wake_up_tasks();

    task_t *chosen_task = scheduler();

    if (chosen_task) {

#ifdef DEBUG
      printf("[i] DEBUG scheduler found task id %d\n", chosen_task->id);
#endif
      chosen_task->quantum = SCHED_QUANTUM;

      // Execute task.
      unsigned int time_started = systime();

      task_switch(chosen_task);

      unsigned int time_spent = systime() - time_started;
      chosen_task->cpu_time += time_spent;
      dispatcher_acc_negative_time += time_spent;
      // Task yielded/exitted.

#ifdef DEBUG
      printf("[i] DEBUG dispatcher received processor, last task id %d with "
             "status %d",
             chosen_task->id, chosen_task->status);
      queue_print("", (queue_t *)task_queue,
                  (void (*)(void *))print_task_queue_aux);
#endif

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
        // do nothing
        break;
      }
    }
  }

  dispatcher_task.cpu_time =
      systime() - dispatcher_start_time - dispatcher_acc_negative_time;

  print_stats();
  task_switch(&main_task);
}

void tick(int signum) {
#ifdef DEBUG
  // printf("[i] DEBUG tick %d\n", current_time);
#endif

  current_time++;
  if (preemption_lock) {
#ifdef DEBUG
    printf("[i] DEBUG tick preemption is locked\n");
#endif
    return;
  }

  if (current_task->status == TASK_SUSPENDED) {
#ifdef DEBUG
    printf("[i] DEBUG tick preempting SUSpended task\n");
#endif
    return;
  }

  if (current_task->preemptable && !(current_task->quantum--)) {
#ifdef DEBUG
    printf("[i] DEBUG tick preempting task id %d with status %d\n",
           current_task->id, current_task->status);
#endif
    current_task->quantum = SCHED_QUANTUM;
    task_yield();
  }
}

unsigned int systime() { return current_time; }

static void print_stats() {
  printf("Task %5d exit: execution time %7d ms, processor time %7d ms, %7d "
         "activations\n",
         current_task->id, systime() - current_task->start_time,
         current_task->cpu_time, current_task->activations);
}

void task_suspend(task_t **queue) {
#ifdef DEBUG
  printf("[i] DEBUG suspending task id %d on queue: \n", current_task->id);
  // queue_print("", (queue_t *)task_queue,
  //             (void (*)(void *))print_task_queue_aux);
#endif
  current_task->status = TASK_SUSPENDED;

  queue_remove((queue_t **)&task_queue, (queue_t *)current_task);

  queue_append((queue_t **)queue, (queue_t *)current_task);

  task_yield();
}

void task_resume(task_t *task, task_t **queue) {
#ifdef DEBUG
  printf("[i] DEBUG resuming task id %d from task id %d on queue: ", task->id,
         current_task->id);
  queue_print("", (queue_t *)*queue, (void (*)(void *))print_task_queue_aux);
#endif
  current_task->status = TASK_READY;
  queue_remove((queue_t **)queue, (queue_t *)task);
  queue_append((queue_t **)&task_queue, (queue_t *)task);
}

int task_join(task_t *task) {
#ifdef DEBUG
  printf("[i] DEBUG join, task %d is waiting task id %d\n", current_task->id,
         task->id);
#endif
  if (!task)
    return -1;

  if (task->status != TASK_TERMINATED)
    task_suspend(&task->join_queue);

#ifdef DEBUG
  printf("[i] DEBUG join, task %d is joined by task id %d\n", current_task->id,
         task->id);
#endif

  return current_task->join_return_code;
}

void task_sleep(int t) {
  current_task->wake_up_time = systime() + t;

  task_suspend(&sleep_queue);
}

void enter_spinlock(int *lock) {
#ifdef DEBUG
  printf("[i] DEBUG task id %d waiting for spinlock\n", task_id());
#endif
  while (__sync_fetch_and_or(lock, 1))
    ; // enter
#ifdef DEBUG
  printf("[i] DEBUG task id %d got spinlock\n", task_id());
#endif
}

void leave_spinlock(int *lock) {
#ifdef DEBUG
  printf("[i] DEBUG task id %d is lefting spinlock\n", task_id());
#endif
  *lock = 0; // leave
#ifdef DEBUG
  printf("[i] DEBUG task id %d left spinlock\n", task_id());
#endif
}

int sem_create(semaphore_t *s, int value) {
#ifdef DEBUG
  printf("[i] DEBUG creating semaphore with initial value %d\n", value);
#endif
  s->counter = value;
  s->wait_queue = NULL;
  s->lock = 0;

  return 0;
}

int sem_down(semaphore_t *s) {
#ifdef DEBUG
  printf("[i] DEBUG downing semaphore with counter %d\n", s->counter);
#endif

  if (!s) {
#ifdef DEBUG
  printf("[i] DEBUG sem_down: semaphore is NULL\n");
#endif
    return -1;
  }

  enter_spinlock(&(s->lock));

  s->counter--;

  if (s->counter < 0) {
#ifdef DEBUG
    printf("[i] DEBUG   semaphore full, task id %d will wait \n",
           current_task->id);
#endif
    current_task->status = TASK_SUSPENDED;

    queue_remove((queue_t **)&task_queue, (queue_t *)current_task);
    queue_append((queue_t **)&(s->wait_queue), (queue_t *)current_task);

    leave_spinlock(&(s->lock));
    task_yield();
  } else {
    leave_spinlock(&(s->lock));
    current_task->sem_return = 0;
  }

#ifdef DEBUG
  printf("[i] DEBUG sem_down() ended, semaphore with counter %d\n", s->counter);
#endif

  return current_task->sem_return;
}

int sem_up(semaphore_t *s) {
#ifdef DEBUG
  printf("[i] DEBUG upping semaphore with counter %d\n", s->counter);
#endif

  if (!s) {
    return -1;
  }

  enter_spinlock(&(s->lock));

  s->counter++;

  if (s->counter <= 0) {
    task_t *fst = s->wait_queue;
    fst->status = TASK_READY;
    fst->sem_return = 0;

    queue_remove((queue_t **)&(s->wait_queue), (queue_t *)fst);
    queue_append((queue_t **)&task_queue, (queue_t *)fst);

#ifdef DEBUG
    printf("[i] DEBUG semaphore was less than zero so resuming task %d\n",
           s->counter, fst->id);
#endif
  }

  leave_spinlock(&(s->lock));

#ifdef DEBUG
  printf("[i] DEBUG upped semaphore now with counter %d\n", s->counter);
#endif

  return 0;
}

int sem_destroy(semaphore_t *s) {
#ifdef DEBUG
  printf("[i] DEBUG destroing semaphore with counter %d\n", s->counter);
  queue_print("", (queue_t *)s->wait_queue,
              (void (*)(void *))print_task_queue_aux);
#endif

  while (s->wait_queue) {
    task_t *curr = s->wait_queue;
    curr->sem_return = -1;
    task_resume(curr, &s->wait_queue);
  }

  return 0;
}

// cria uma fila para até max mensagens de size bytes cada
int mqueue_create(mqueue_t *queue, int max, int size) {
  queue->buffer = calloc(max, size);
  if (!queue->buffer) {
#ifdef DEBUG
    printf("[i] DEBUG mqueue_create could not allocate memory for message "
           "queue buffer\n");
#endif

    return -1;
  }

  if (sem_create(&queue->s_item, 0) || sem_create(&queue->s_vaga, max) ||
      sem_create(&queue->s_buff, 1)) {
#ifdef DEBUG
    printf("[i] DEBUG mqueue_create could not create semaphores for message "
           "queue\n");
#endif

    return -1;
  }

  queue->write_pos = 0;
  queue->read_pos = 0;
  queue->num_msgs = 0;
  queue->buffer_capacity = max;
  queue->datatype_size = size;

  return 0;
}

// envia uma mensagem para a fila
int mqueue_send(mqueue_t *queue, void *msg) {
  if (!queue || !queue->buffer || !msg) {
#ifdef DEBUG
    printf("[i] DEBUG mqueue_send wrong arguments, something is NULL\n");
#endif

    return -1;
  }

  if (sem_down(&queue->s_vaga)) return -1;
  if (sem_down(&queue->s_buff)) return -1;

  ptrdiff_t pos = queue->write_pos * queue->datatype_size;
  memcpy(queue->buffer + pos, msg, queue->datatype_size);

  queue->write_pos = (queue->write_pos + 1) % queue->buffer_capacity;

  queue->num_msgs++;

  if (sem_up(&queue->s_buff)) return -1;
  if (sem_up(&queue->s_item)) return -1;
  
  return 0;
}

// recebe uma mensagem da fila
int mqueue_recv(mqueue_t *queue, void *msg) {
  if (!queue || !queue->buffer || !msg) {
#ifdef DEBUG
    printf("[i] DEBUG mqueue_recv wrong arguments, something is NULL\n");
#endif
    
    return -1;
  }

  if (sem_down(&queue->s_item)) return -1;
  if (sem_down(&queue->s_buff)) return -1;

  ptrdiff_t pos = queue->read_pos * queue->datatype_size;
  memcpy(msg, queue->buffer + pos, queue->datatype_size);

  queue->read_pos = (queue->read_pos + 1) % queue->buffer_capacity;

  queue->num_msgs--;

  if (sem_up(&queue->s_buff)) return -1;
  if (sem_up(&queue->s_vaga)) return -1;

  return 0;
}

// destroi a fila, liberando as tarefas bloqueadas
int mqueue_destroy(mqueue_t *queue) {
  if (!queue || !queue->buffer) {
#ifdef DEBUG
    printf("[i] DEBUG mqueue_destroy wrong arguments, something is NULL\n");
#endif
    
    return -1;
  }

    sem_destroy(&queue->s_item);
    sem_destroy(&queue->s_vaga);
    sem_destroy(&queue->s_buff);

    queue->buffer = NULL;

    return 0;
}

// informa o número de mensagens atualmente na fila
int mqueue_msgs(mqueue_t *queue) {
  if (!queue) {
#ifdef DEBUG
    printf("[i] DEBUG mqueue_msgs wrong arguments, something is NULL\n");
#endif
    
    return -1;
  }

  return queue->num_msgs;
}
