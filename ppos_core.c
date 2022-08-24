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

struct sigaction preemption_action;
struct itimerval preemption_timer = {0};

task_t *scheduler();
void dispatcher();
void tick(int signum);
static void print_stats();

void *print_task_queue_aux(queue_t *q) {
  task_t *t = (task_t *)q;
  printf("%3d:%-4u", t->id, t->wake_up_time);

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
  printf("[i] DEBUG switching tasks from id %d to id %d\n", current_task->id,
         task->id);
#endif

  task->activations++;

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
  queue_print("[i] DEBUG scheduler task queue", (queue_t *)task_queue,
              (void (*)(void *))print_task_queue_aux);
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

      // Execute task.
      unsigned int time_started = systime();

      task_switch(chosen_task);

      unsigned int time_spent = systime() - time_started;
      chosen_task->cpu_time += time_spent;
      dispatcher_acc_negative_time += time_spent;
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
  // printf("[i] DEBUG tick\n");
#endif

  current_time++;

  if (current_task->preemptable && !(current_task->quantum--)) {
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
  //queue_print("", (queue_t *)task_queue,
  //            (void (*)(void *))print_task_queue_aux);
#endif
  current_task->status = TASK_SUSPENDED;

  queue_remove((queue_t **)&task_queue, (queue_t *)current_task);

  queue_append((queue_t **)queue, (queue_t *)current_task);

  task_yield();
}

void task_resume(task_t *task, task_t **queue) {
#ifdef DEBUG
  printf("[i] DEBUG resuming task id %d on queue: \n", current_task->id);
  //queue_print("", (queue_t *)task_queue,
  //            (void (*)(void *))print_task_queue_aux);
#endif
  current_task->status = TASK_READY;
  queue_remove((queue_t **)queue, (queue_t *)task);
  queue_append((queue_t **)&task_queue, (queue_t *)task);
}

int task_join(task_t *task) {
  if (!task)
    return -1;

  if (task->status != TASK_TERMINATED)
    task_suspend(&task->join_queue);

  return current_task->join_return_code;
}

void task_sleep(int t) {
  current_task->wake_up_time = systime() + t;

  task_suspend(&sleep_queue);
}

int sem_create(semaphore_t *s, int value) {
  s->counter = value;
  s->wait_queue = NULL;
  s->lock = 0;

  return 1;
}

int sem_down(semaphore_t *s) {
  if (!s)
    return -1;

  // spinlock
  while (__sync_fetch_and_or(&s->lock, 1))
    ; // enter
  // printf("DOWN %d: counter %d\n", task_id(), s->counter);
  s->counter--;

  if (s->counter < 0) {
    // printf("DOWN %d: will wait...\n", task_id());
    s->lock = 0; // leave
    task_suspend(&s->wait_queue);
  } else {
    // printf("DOWN %d: will not wait!!!\n", task_id());
    s->lock = 0; // leave
    current_task->sem_return = 0;
  }

  return current_task->sem_return;
}

int sem_up(semaphore_t *s) {
  if (!s || !s->wait_queue)
    return -1;

  // printf("UP %d: uppded\n", task_id());

  // spinlock
  while (__sync_fetch_and_or(&(s->lock), 1))
    ; // enter
  s->counter++;

  if (s->counter <= 0) {
    // printf("UP %d: releasing task %d\n", task_id(), s->wait_queue->id);
    task_t *curr = s->wait_queue;
    curr->sem_return = 0;
    task_resume(curr, &s->wait_queue);
  }

  s->lock = 0; // leave

  return 0;
}

int sem_destroy(semaphore_t *s) {
  printf("destruindo s com counter: %d\n", s->counter);

  while (s->wait_queue) {
    task_t *curr = s->wait_queue;
    curr->sem_return = -1;
    task_resume(curr, &s->wait_queue);
  }

  return 0;
}
