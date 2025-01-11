#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include "src/common/io.h"
#include "src/common/constants.h"

#include "client_str.h"
#include "constants.h"
#include "io.h"
#include "link_lst.h"
#include "operations.h"
#include "parser.h"
#include "pthread.h"

struct SharedData {
  DIR *dir;
  char *dir_name;
  pthread_mutex_t directory_mutex;
};

// Estrutura para armazenar os descritores de arquivos do cliente
typedef struct {
  int req_fd;     // Descritor de arquivo para o FIFO de requisições
  int resp_fd;    // Descritor de arquivo para o FIFO de respostas
  int notif_fd;   // Descritor de arquivo para o FIFO de notificações
} client_data_t;


pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t n_current_backups_lock = PTHREAD_MUTEX_INITIALIZER;

size_t active_backups = 0; // Number of active backups
size_t max_backups;        // Maximum allowed simultaneous backups
size_t max_threads;        // Maximum allowed simultaneous threads
char *jobs_directory = NULL;
char *fifo_regist_name = NULL;//m

int filter_job_files(const struct dirent *entry) {
  const char *dot = strrchr(entry->d_name, '.');
  if (dot != NULL && strcmp(dot, ".job") == 0) {
    return 1; // Keep this file (it has the .job extension)
  }
  return 0;
}

static int entry_files(const char *dir, struct dirent *entry, char *in_path,
                       char *out_path) {
  const char *dot = strrchr(entry->d_name, '.');
  if (dot == NULL || dot == entry->d_name || strlen(dot) != 4 ||
      strcmp(dot, ".job")) {
    return 1;
  }

  if (strlen(entry->d_name) + strlen(dir) + 2 > MAX_JOB_FILE_NAME_SIZE) {
    fprintf(stderr, "%s/%s\n", dir, entry->d_name);
    return 1;
  }

  strcpy(in_path, dir);
  strcat(in_path, "/");
  strcat(in_path, entry->d_name);

  strcpy(out_path, in_path);
  strcpy(strrchr(out_path, '.'), ".out");

  return 0;
}

static int run_job(int in_fd, int out_fd, char *filename) {
  size_t file_backups = 0;
  while (1) {
    char keys[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    char values[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    unsigned int delay;
    size_t num_pairs;

    switch (get_next(in_fd)) {
    case CMD_WRITE:
      num_pairs =
          parse_write(in_fd, keys, values, MAX_WRITE_SIZE, MAX_STRING_SIZE);
      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_write(num_pairs, keys, values)) {
        write_str(STDERR_FILENO, "Failed to write pair\n");
      }
      break;

    case CMD_READ:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_read(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to read pair\n");
      }
      break;

    case CMD_DELETE:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_delete(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to delete pair\n");
      }
      break;

    case CMD_SHOW:
      kvs_show(out_fd);
      break;

    case CMD_WAIT:
      if (parse_wait(in_fd, &delay, NULL) == -1) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay > 0) {
        printf("Waiting %d seconds\n", delay / 1000);
        kvs_wait(delay);
      }
      break;

    case CMD_BACKUP:
      pthread_mutex_lock(&n_current_backups_lock);
      if (active_backups >= max_backups) {
        wait(NULL);
      } else {
        active_backups++;
      }
      pthread_mutex_unlock(&n_current_backups_lock);
      int aux = kvs_backup(++file_backups, filename, jobs_directory);

      if (aux < 0) {
        write_str(STDERR_FILENO, "Failed to do backup\n");
      } else if (aux == 1) {
        return 1;
      }
      break;

    case CMD_INVALID:
      write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
      break;

    case CMD_HELP:
      write_str(STDOUT_FILENO,
                "Available commands:\n"
                "  WRITE [(key,value)(key2,value2),...]\n"
                "  READ [key,key2,...]\n"
                "  DELETE [key,key2,...]\n"
                "  SHOW\n"
                "  WAIT <delay_ms>\n"
                "  BACKUP\n" // Not implemented
                "  HELP\n");

      break;

    case CMD_EMPTY:
      break;

    case EOC:
      printf("EOF\n");
      return 0;
    }
  }
}

// frees arguments
static void *get_file(void *arguments) {
  struct SharedData *thread_data = (struct SharedData *)arguments;
  DIR *dir = thread_data->dir;
  char *dir_name = thread_data->dir_name;

  if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to lock directory_mutex\n");
    return NULL;
  }

  struct dirent *entry;
  char in_path[MAX_JOB_FILE_NAME_SIZE], out_path[MAX_JOB_FILE_NAME_SIZE];
  while ((entry = readdir(dir)) != NULL) {
    if (entry_files(dir_name, entry, in_path, out_path)) {
      continue;
    }

    if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to unlock directory_mutex\n");
      return NULL;
    }

    int in_fd = open(in_path, O_RDONLY);
    if (in_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open input file: ");
      write_str(STDERR_FILENO, in_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open output file: ");
      write_str(STDERR_FILENO, out_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out = run_job(in_fd, out_fd, entry->d_name);

    close(in_fd);
    close(out_fd);

    if (out) {
      if (closedir(dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
      }

      exit(0);
    }

    if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to lock directory_mutex\n");
      return NULL;
    }
  }

  if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to unlock directory_mutex\n");
    return NULL;
  }

  pthread_exit(NULL);
}


/** m
 * This function opens the pipes the client created,
 * and deals with client requests and answers
 * 
 * @param  buffer buffer with connect operation
 * @return 0 if everithing well, and the client disconnected
 * @return 1 if some problem
 */
int serve_client(char *buffer){

  int req_fd, resp_fd, notif_fd;
  char subbuffer[41];
  Client client;

  if (buffer[0] != '1'){
    fprintf(stderr, "Wrong operation number, should've been <1> was <%c>\n", buffer[0]);
    return 1;
  }

  //m connect to requests pipe
  strncpy(subbuffer, buffer + 1, 40);
  subbuffer[40] = '\0';
  req_fd = open(subbuffer, O_RDONLY);
  if (req_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    return 1;
  }
  
  //m connect to answers pipe
  strncpy(subbuffer, buffer + 1 + 40, 40);
  subbuffer[40] = '\0';
  resp_fd = open(subbuffer, O_WRONLY);
  if (resp_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    close(req_fd);
    return 1;
  }
  
  //m connect to notifications pipe
  strncpy(subbuffer, buffer + 1 + 40 + 40, 40);
  subbuffer[40] = '\0';
  notif_fd = open(subbuffer, O_WRONLY);
  if (notif_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    close(req_fd);
    close(resp_fd);
    return 1;
  }

  client.req_fd = req_fd;
  client.resp_fd = resp_fd;
  client.notif_fd = notif_fd;

  int error = 0;
  while (1){
    //m get commands
    char op;
    read(req_fd, &op, 1);
    if (op == '2') {
      if(kvs_disconnect_client(&client) != 0){
        fprintf(stderr, "Error disconnecting the client from the kvs table\n");
        error = 1;
      }
      break;
    }
    else if (op == '3') {
      if(kvs_subscribe_key(&client) != 0){
        error = 1;
        break;
      }
    }
    else if (op == '4') {
      if(kvs_unsubscribe_key(&client) != 0){
        error = 1;
        break;
      }
    }
    else{
      fprintf(stderr,
        "Wrong operation number, should've been 1, 2 or 3 but was <%c>\n", op);
      error = 1;
      break;
    }
  }

  //m destroy list of keys of the client
  destroy_str_list(client.keys);

  //m not sure, but i think it makes sense to not only clean subscribtions but also close pipes connect..
  close(req_fd);
  close(resp_fd);
  close(notif_fd);

  return error;
  }

/**
 * This function handles the connecting requests,
 * and then handles the client(its requests and answers)
 */
int get_requests(char *regist_pipe_path){ //estava void
  //m create and open requests
  int error = 0;

  if (mkfifo(regist_pipe_path, 0666) == -1) {
    if (errno != EEXIST) {
      fprintf(stderr, "Failed to create regists thread\n");
      return 1;
    }
  }

  // Abrir o FIFO de registo para ler as conexões dos clientes
  int regist_fd = open(regist_pipe_path, O_RDONLY);
  if (regist_fd == -1) {
    fprintf(stderr,"Failed to open fifo <%s> for reading\n", regist_pipe_path);
    unlink(regist_pipe_path);
    return 1;
  }
  while (1){ //right know unless theres an error this while is infinite, it keeps reading connections
    //m gets a client and connects to the clients pipes
    char buffer[1 + 40 + 40 + 40];
    int intr;
    int value = read_all(regist_fd, buffer, 1 + 40 + 40 + 40, &intr);
    
    if (intr == 1){
      fprintf(stderr, "Reading from regist pipe was interupted\n");
      error = 1;
      break;
    }
    else if (value == -1){
      fprintf(stderr, "There was an error while reading from regist pipe\n");
      error = 1;
      break;
    }
    else if (value == 0){
      continue; //wait for the next client connection
    }

    if (serve_client(buffer) != 0){
      fprintf(stderr,"There was an error with the client\n");
      error = 1;
      break;
    }

  }

  // Criar a estrutura para passar os dados de clientes
  // Passar regist_fd e dir para a função de despachar threads
  // Fechar e remover o FIFO de registo
  close(regist_fd);

  if(unlink(regist_pipe_path) != 0){
    fprintf(stderr, "Failed to destroy fifo <%s>\n", regist_pipe_path);
    return 1;
  }
  
  return error;
}

static void dispatch_threads(DIR *dir, char *regist_path) {
  pthread_t *threads = malloc(max_threads * sizeof(pthread_t));

  if (threads == NULL) {
    fprintf(stderr, "Failed to allocate memory for threads\n");
    return;
  }

  struct SharedData thread_data = {dir, jobs_directory, PTHREAD_MUTEX_INITIALIZER};

  for (size_t i = 0; i < max_threads; i++) {
    if (pthread_create(&threads[i], NULL, get_file, (void *)&thread_data) != 0) {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      continue; // Se a thread não for criada, pulamos para o próximo cliente
    }
  }


  // ler do FIFO de registo
  get_requests(regist_path);


  for (unsigned int i = 0; i < max_threads; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
      fprintf(stderr, "Failed to join thread %u\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  if (pthread_mutex_destroy(&thread_data.directory_mutex) != 0) {
    fprintf(stderr, "Failed to destroy directory_mutex\n");
  }

  free(threads);
}


int main(int argc, char **argv) {
  if (argc < 4) {
    write_str(STDERR_FILENO, "Usage: ");
    write_str(STDERR_FILENO, argv[0]);
    write_str(STDERR_FILENO, " <jobs_dir>");
    write_str(STDERR_FILENO, " <max_threads>");
    write_str(STDERR_FILENO, " <max_backups> \n");
    return 1;
  }

  jobs_directory = argv[1];
  /////////////////////////////////////////////////////////////
  
  /////////////////////////////////////////////////////////////
  char *endptr;
  max_backups = strtoul(argv[3], &endptr, 10);

  if (*endptr != '\0') {
    fprintf(stderr, "Invalid max_proc value\n");
    return 1;
  }

  max_threads = strtoul(argv[2], &endptr, 10);

  if (*endptr != '\0') {
    fprintf(stderr, "Invalid max_threads value\n");
    return 1;
  }

  if (max_backups <= 0) {
    write_str(STDERR_FILENO, "Invalid number of backups\n");
    return 0;
  }

  if (max_threads <= 0) {
    write_str(STDERR_FILENO, "Invalid number of threads\n");
    return 0;
  }

  if (kvs_init()) {
    write_str(STDERR_FILENO, "Failed to initialize KVS\n");
    return 1;
  }

  DIR *dir = opendir(argv[1]);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
    return 0;
  }

  dispatch_threads(dir, argv[4]);

  if (closedir(dir) == -1) {
    fprintf(stderr, "Failed to close directory\n");
    return 0;
  }

  while (active_backups > 0) {
    wait(NULL);
    active_backups--;
  }

  kvs_terminate();

  return 0;
}
