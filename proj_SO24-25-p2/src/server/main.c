#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#include <string.h>
#include <signal.h>

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


//pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t n_current_backups_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t sem_lock = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_prod; //semaforo dos produtors, vai ser inicializado com tamanho maximo
sem_t sem_consum; //semaforo dos consumidores
//we put the request in the buffer of productor-consumer
//char *buffer_p_c[MAX_SESSION_COUNT]; //we will use strdup for the strings, so no problem
char buffer_p_c[MAX_SESSION_COUNT][1 + 40 + 40 + 40 + 1]; //we will use strdup for the strings, so no problem
int i_prod = 0;
int i_consum = 0;

size_t active_backups = 0; // Number of active backups
size_t max_backups;        // Maximum allowed simultaneous backups
size_t max_threads;        // Maximum allowed simultaneous threads
char *jobs_directory = NULL;
char *fifo_regist_name = NULL;//m
char *regist_pipe_path_sig = NULL;



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
    printf("e1\n");
    char keys[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    char values[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    unsigned int delay;
    size_t num_pairs;

    switch (get_next(in_fd)) {
    case CMD_WRITE:
      printf("e2\n");

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
      printf("e3\n");
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
      printf("e4\n");
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);
      printf("e41\n");

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }
      printf("e422\n");
      if (kvs_delete(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to delete pair\n");
      }
      printf("e42\n");

      break;

    case CMD_SHOW:
      printf("e5\n");
      kvs_show(out_fd);
      break;

    case CMD_WAIT:
      printf("e6\n");
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
      printf("e7\n");
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
  printf("dir_name= %s\n", dir_name);
  if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to lock directory_mutex\n");
    return NULL;
  }
  printf("a\n");
  struct dirent *entry;
  char in_path[MAX_JOB_FILE_NAME_SIZE], out_path[MAX_JOB_FILE_NAME_SIZE];
  while ((entry = readdir(dir)) != NULL) {
    if (entry_files(dir_name, entry, in_path, out_path)) {
      continue;
    }
    printf("b\n");
    if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to unlock directory_mutex\n");
      return NULL;
    }
    printf("c\n");
    int in_fd = open(in_path, O_RDONLY);
    if (in_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open input file: ");
      write_str(STDERR_FILENO, in_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }
    printf("d\n");
    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open output file: ");
      write_str(STDERR_FILENO, out_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }
    printf("e\n");
    int out = run_job(in_fd, out_fd, entry->d_name);

    close(in_fd);
    close(out_fd);
    printf("f\n");

    if (out) {
      if (closedir(dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
      }

      exit(0);
    }
    printf("g\n");

    if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to lock directory_mutex\n");
      return NULL;
    }
  }
  printf("e\n");

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

/*int serve_client_old(char *buffer){
  printf("cc1\n");
  int req_fd, resp_fd, notif_fd;
  char subbuffer[41];
  client_init(&client);
  printf("cc2\n");

  if (buffer[0] != '1'){
    fprintf(stderr, "Wrong operation number, should've been <1> was <%c>\n", buffer[0]);
    return 1;
  }
  printf("cc3\n");

  //m connect to requests pipe
  strncpy(subbuffer, buffer + 1, 40);
  subbuffer[40] = '\0';
  req_fd = open(subbuffer, O_RDONLY);
  if (req_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    return 1;
  }
  printf("cc4\n");
  
  //m connect to answers pipe
  strncpy(subbuffer, buffer + 1 + 40, 40);
  subbuffer[40] = '\0';
  resp_fd = open(subbuffer, O_WRONLY);
  if (resp_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    close(req_fd);
    return 1;
  }
  printf("cc5\n");
  
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
  printf("cc6\n");

  write(resp_fd, "10", 2);

  client.req_fd = req_fd;
  client.resp_fd = resp_fd;
  client.notif_fd = notif_fd;

  int error = 0;
  while (1){
    printf("cc7\n");

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
  }*/


/** m
 * This function opens the pipes the client created,
 * and deals with client requests and answers
 * 
 * @param  buffer buffer with connect operation
 * @return 0 if everithing well, and the client disconnected
 * @return 1 if some problem
 */
void *serve_client(){
  int req_fd, resp_fd, notif_fd;
  char subbuffer[41];

  //wait for space to consum
  sem_wait(&sem_consum);
  pthread_mutex_lock(&sem_lock);
  
  //get a client info request from the buffer
  char buffer[1 + 40 + 40 + 40 + 1];
  printf("iconsum = %d\n", i_consum);
  memcpy(buffer, buffer_p_c[i_consum], 1 + 40 + 40 + 40 + 1); //cant forget to free at the end
  i_consum = (i_consum + 1)%MAX_SESSION_COUNT;
  printf("iconsum = %d\n", i_consum);
  printf("buffer= <%s>\n", buffer);
  char path[41];
  strncpy(path, buffer + 41, 41);
  printf("buffer= <%s>\n", path);
  strncpy(path, buffer + 81, 41);
  printf("buffer= <%s>\n", path);
  for(int i = 0; i< 121; i++){
    printf("i: %d <%c>\n", i, buffer[i]);
  }

  Client *client = create_client(); //this function will create a client and add it to a list of clients online, it returns a pointer to it
  add_Client(client); //adds client to server list of clients in operations
  printf("cc1\n");
  
  //client_init(&client);
  printf("cc2\n");

  if (buffer[0] != '1'){
    fprintf(stderr, "Wrong operation number, should've been <1> was <%c>\n", buffer[0]);
  }
  printf("cc3\n");

  //m connect to requests pipe
  strncpy(subbuffer, buffer + 1, 40);
  subbuffer[40] = '\0';
  req_fd = open(subbuffer, O_RDONLY);
  if (req_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
  }
  printf("cc4\n");
  
  //m connect to answers pipe
  strncpy(subbuffer, buffer + 1 + 40, 40);
  subbuffer[40] = '\0';
  resp_fd = open(subbuffer, O_WRONLY);
  if (resp_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
  }
  printf("cc5\n");
  
  //m connect to notifications pipe
  strncpy(subbuffer, buffer + 1 + 40 + 40, 40);
  subbuffer[40] = '\0';
  notif_fd = open(subbuffer, O_WRONLY);
  if (notif_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
  }
  printf("cc6\n");
  
  //free buffer that was created by prod using strdup
  //free(buffer); 

  //give feedback to client about successful connection to server
  write(resp_fd, "10", 2);

  client->req_fd = req_fd;
  client->resp_fd = resp_fd;
  client->notif_fd = notif_fd;
  
  while (1){
    printf("cc7\n");

    //m get commands
    char op;
    read(req_fd, &op, 1);
    if (op == '2') {
      if(kvs_disconnect_client(client) != 0){
        fprintf(stderr, "Error disconnecting the client from the kvs table\n");
      }
      break;
    }
    else if (op == '3') {
      if(kvs_subscribe_key(client) != 0){
        break;
      }
    }
    else if (op == '4') {
      if(kvs_unsubscribe_key(client) != 0){
        break;
      }
    }
    else{
      fprintf(stderr,
        "Wrong operation number, should've been 1, 2 or 3 but was <%c>\n", op);
      break;
    }
  }


  //m not sure, but i think it makes sense to not only clean subscribtions but also close pipes connect..
  close(req_fd);
  close(resp_fd);
  close(notif_fd);

  return NULL;
  }

/**
 * This function handles the connecting requests,
 * and then handles the client(its requests and answers)
 */

/*int get_requests_old(char *regist_pipe_path){ //estava void
  //m create and open requests
  int error = 0;
  //init semaphores
  //creating regist fifo
  printf("aaa\n");
  printf("aa\n");
  umask(0);
  if (mkfifo(regist_pipe_path, 0666) == -1) {
    perror("mkfifo failed");
    if (errno != EEXIST) {
      fprintf(stderr, "Failed to create regists thread\n");
      return 1;
    }
    printf("ERROR\n");
  }
  printf("a\n");

  // opening regist fifo
  printf("regist path= <%s>\n", regist_pipe_path);
  int regist_fd = open(regist_pipe_path, O_RDONLY);
  printf("ab\n");
  if (regist_fd == -1) {
    fprintf(stderr,"Failed to open fifo <%s> for reading\n", regist_pipe_path);
    unlink(regist_pipe_path);
    return 1;
  }
  printf("b\n");

  //getting requests
  char buffer[1 + 40 + 40 + 40]; //for clients request info
  int intr = 0; //not used for now
  while (1){ 
    //wait for space to add new request
    
    //int value = read_all(regist_fd, buffer, 1 + 40 + 40 + 40, &intr);
    int value = (int)read(regist_fd, buffer, 1 + 40 + 40 + 40);
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

    printf("c\n");
    //putting info in the prod-consum buffer
    if (serve_client(buffer) != 0){
      fprintf(stderr,"There was an error with the client\n");
      error = 1;
      break;
    }
    printf("d\n");

    printf("wait for new client request\n");

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
}*/


int get_requests(char *regist_pipe_path){ //estava void
  //m create and open requests
  int error = 0;
  //init semaphores
  sem_init(&sem_prod, 0, MAX_SESSION_COUNT);
  sem_init(&sem_consum, 0, 0);

  //creating regist fifo
  printf("aaa\n");
  printf("aa\n");
  umask(0);
  if (mkfifo(regist_pipe_path, 0666) == -1) {
    perror("mkfifo failed");
    if (errno != EEXIST) {
      fprintf(stderr, "Failed to create regists thread\n");
      return 1;
    }
    printf("ERROR\n");
  }
  printf("a\n");

  // opening regist fifo
  printf("regist path= <%s>\n", regist_pipe_path);
  int regist_fd = open(regist_pipe_path, O_RDONLY);
  printf("ab\n");
  if (regist_fd == -1) {
    fprintf(stderr,"Failed to open fifo <%s> for reading\n", regist_pipe_path);
    unlink(regist_pipe_path);
    return 1;
  }
  printf("b\n");

  int thread_id;
  //sending client handling threads
  pthread_t threads[MAX_SESSION_COUNT];
  for (size_t i = 0; i < max_threads; i++) {
    if (pthread_create(&threads[i], NULL, serve_client, NULL) != 0) {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      error = 1;
      continue;
    }
    thread_id = (int)i;
    printf("Thread [%d] starting\n", thread_id);
  }
  
  //getting requests
  char buffer[1 + 40 + 40 + 40 + 1]; //for clients request info
  int intr = 0; //not used for now
  while (1){ 
    //wait for space to add new request
    sem_wait(&sem_prod);
    pthread_mutex_lock(&sem_lock);
    
    //int value = read_all(regist_fd, buffer, 1 + 40 + 40 + 40, &intr);
    int value = (int)read(regist_fd, buffer, 1 + 40 + 40 + 40);
    buffer[121] = '\0';
    printf("[%d]buffer= <%s>\n", thread_id, buffer);
    char path[41];
    strncpy(path, buffer + 41, 41);
    printf("[%d]buffer= <%s>\n", thread_id, path);
    strncpy(path, buffer + 81, 41);
    printf("[%d]buffer= <%s>\n", thread_id, path);
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

    //adding to prod-consum buffer
    //char *dup = strdup(buffer);
    

    memcpy(buffer_p_c[i_prod], buffer, 1 + 40 + 40 + 40 + 1);

    printf("[%d]buff_p_c= <%s>\n", thread_id, buffer_p_c[i_prod]);
    strncpy(path, buffer_p_c[i_prod] + 41, 41);
    printf("[%d]buff_p_c= <%s>\n", thread_id, path);
    strncpy(path, buffer_p_c[i_prod] + 81, 41);
    printf("[%d]buff_p_c= <%s>\n", thread_id, path);

    printf("[%d]iprod = %d\n", thread_id, i_prod);
    i_prod = (i_prod + 1)%MAX_SESSION_COUNT;
    printf("[%d]iprod = %d\n", thread_id, i_prod);
    
    
    
    printf("[%d]c\n", thread_id);

    pthread_mutex_unlock(&sem_lock);
    sem_post(&sem_consum); //new client to attend
    
    printf("[%d]wait for new client request\n", thread_id);
  }
  
  

  // Fechar e remover o FIFO de registo
  close(regist_fd);

  if(unlink(regist_pipe_path) != 0){
    fprintf(stderr, "Failed to destroy fifo <%s>\n", regist_pipe_path);
    return 1;
  }

  //wait for threads
  for (unsigned int i = 0; i < max_threads; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
      fprintf(stderr, "Failed to join thread %u\n", i);
      error = 1;
    }
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

  printf("created\n");
  // ler do FIFO de registo
  get_requests(regist_path);
  printf("after\n");


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

void sigtstp_handler() {
    printf("\nCaught SIGTSTP (Ctrl+Z). Cleaning up resources...\n");
    if(unlink(regist_pipe_path_sig) != 0){
      fprintf(stderr, "Failed to destroy fifo <%s>\n", regist_pipe_path_sig);
    }
    exit(0);
}
int main(int argc, char **argv) {
  signal(SIGTSTP, sigtstp_handler);
  if (argc < 4) {
    write_str(STDERR_FILENO, "Usage: ");
    write_str(STDERR_FILENO, argv[0]);
    write_str(STDERR_FILENO, " <jobs_dir>");
    write_str(STDERR_FILENO, " <max_threads>");
    write_str(STDERR_FILENO, " <max_backups> \n");
    return 1;
  }

  printf("argv[4] = <%s>\n", argv[4]);
  if(argv[4] == NULL){printf("hell nah\n");}

  jobs_directory = argv[1];
  regist_pipe_path_sig = argv[4];
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
