// PingPongOS - PingPong Operating System
// Prof. Carlos A. Maziero, DINF UFPR
// Versão 1.4 -- Janeiro de 2022

// Estruturas de dados internas do sistema operacional

#ifndef __PPOS_DATA__
#define __PPOS_DATA__

#include <ucontext.h> // biblioteca POSIX de trocas de contexto
#include <stddef.h>

// Estrutura que define um Task Control Block (TCB)
typedef struct task_t {
  struct task_t *prev, *next; // ponteiros para usar em filas
  int id;                     // identificador da tarefa
  ucontext_t context;         // contexto armazenado da tarefa
  short status;               // pronta, rodando, suspensa, ...
  short preemptable;          // pode ser preemptada?
  short quantum;
  short system_task;
  short prio_est;     // prioridade estatica [-20; 20]
  long long prio_din; // prioridade din

  // stats
  unsigned int cpu_time;    // tempo de cpu
  unsigned int start_time;  // tempo de inicio
  unsigned int activations; // ativacoes

  struct task_t *join_queue;

  int join_return_code;

  unsigned int wake_up_time; // tempo que a tarefa deve acordar

  int sem_return; // semaphore return code
  // ... (outros campos serão adicionados mais tarde)
} task_t;

// estrutura que define um semáforo
typedef struct {
  // preencher quando necessário
  task_t *wait_queue;    // fila de espera
  int counter;
  int lock;
} semaphore_t;

// estrutura que define um mutex
typedef struct {
  // preencher quando necessário
} mutex_t;

// estrutura que define uma barreira
typedef struct {
  // preencher quando necessário
} barrier_t;

// estrutura que define uma fila de mensagens
typedef struct {
  void *buffer;

  semaphore_t s_item;
  semaphore_t s_vaga;
  semaphore_t s_buff;

  ptrdiff_t write_pos;
  ptrdiff_t read_pos;

  size_t num_msgs;
  size_t buffer_capacity;
  size_t datatype_size;

} mqueue_t;

#endif
