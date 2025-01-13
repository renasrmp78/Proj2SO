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

sigset_t sigset_anf;

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
  sigset_t sigset;

  sigemptyset(&sigset);
  sigaddset(&sigset, SIGUSR1);

  if (pthread_sigmask(SIG_BLOCK, &sigset, NULL) != 0) {
      fprintf(stderr, "Error blocking the SIGUSR1 signal\n");
      pthread_exit(NULL);
  }

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
void *serve_client(){
  printf("[ServeC] Entered serve_client\n");
  int req_fd, resp_fd, notif_fd;
  char subbuffer[41];
  
  sigset_t sigset;

  sigemptyset(&sigset);
  sigaddset(&sigset, SIGUSR1);

  if (pthread_sigmask(SIG_BLOCK, &sigset, NULL) != 0) {
      fprintf(stderr, "Error blocking the SIGUSR1 signal\n");
      pthread_exit(NULL);
  }

  while (1) {
    //wait for space to consum
    printf("[ServeC] Waiting for a client request\n");
    sem_wait(&sem_consum);
    pthread_mutex_lock(&sem_lock);
    printf("[ServeC] Permision to get client request. Blocking prod-consum mutex\n");
    
    
    //get a client info request from the buffer
    char buffer[1 + 40 + 40 + 40 + 1];
    printf("[ServeC] Index of consumer before= %d\n", i_consum);
    printf("[ServeC] Getting client request from prod-consum buffer to a buffer\n");
    memcpy(buffer, buffer_p_c[i_consum], 1 + 40 + 40 + 40 + 1); //cant forget to free at the end
    i_consum = (i_consum + 1)%MAX_SESSION_COUNT;
    printf("[ServeC] Index of consumer after= %d\n", i_consum);

    printf("[ServeC] Unlocking prod-consum mutex\n");
    pthread_mutex_unlock(&sem_lock);
    sem_post(&sem_prod); //new client to attend
    printf("[ServeC] 1 space added to producer semaphore\n");
    
    char path[41];
    printf("[ServeC] buffer= <%s>\n", buffer);
    strncpy(path, buffer + 41, 41);
    printf("[ServeC] buffer= <%s>\n", path);
    strncpy(path, buffer + 81, 41);
    printf("[ServeC] buffer= <%s>\n", path);

    printf("[ServeC] Creating client object\n");
    Client *client = create_client(); //this function will create a client and add it to a list of clients online, it returns a pointer to it
    printf("[ServeC] Client created. Adding it to the list.\n");
    add_Client(client); //adds client to server list of clients in operations
    printf("[ServeC] Client added to clients list\n");


    printf("[ServeC] Getting buffer information\n");
    if (buffer[0] != '1'){
      fprintf(stderr, "Wrong operation number, should've been <1> was <%c>\n", buffer[0]);
    }
    //m connect to requests pipe
    strncpy(subbuffer, buffer + 1, 40);
    subbuffer[40] = '\0';

    printf("[ServeC] Openning requests fifo for reading\n");
    req_fd = open(subbuffer, O_RDONLY);
    if (errno == ENOENT){ //if client desapeared
      kvs_remove_client(client->id);
      close(req_fd);
      close(resp_fd);
      close(notif_fd);
      continue; //skipp for next client
    }
    else if (req_fd == -1) {
      fprintf(stderr, "Failed to open fifo <%s> for reading\n", subbuffer);
    }
    printf("[ServeC] Openned requests fifo successfully\n");

    //m connect to answers pipe
    strncpy(subbuffer, buffer + 1 + 40, 40);
    subbuffer[40] = '\0';
    
    printf("[ServeC] Openning respostas fifo for writing\n");
    resp_fd = open(subbuffer, O_WRONLY);
    if (errno == ENOENT){ //if client desapeared
      kvs_remove_client(client->id);
      close(req_fd);
      close(resp_fd);
      close(notif_fd);
      continue; //skipp for next client
    }
    else if (resp_fd == -1) {
      fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    }
    printf("[ServeC] Openned respostas fifo successfully\n");

    
    //m connect to notifications pipe
    strncpy(subbuffer, buffer + 1 + 40 + 40, 40);
    subbuffer[40] = '\0';

    printf("[ServeC] Openning notifications fifo for writing\n");
    notif_fd = open(subbuffer, O_WRONLY);
    if (errno == ENOENT){ //if client desapeared
      kvs_remove_client(client->id);
      close(req_fd);
      close(resp_fd);
      close(notif_fd);
      continue; //skipp for next client
    }
    else if (notif_fd == -1) {
      fprintf(stderr, "Failed to open fifo <%s> for writing\n", subbuffer);
    }
      printf("[ServeC] Openned notifications fifo successfully\n");
    
    //free buffer that was created by prod using strdup
    //free(buffer); 

    //give feedback to client about successful connection to server
    printf("[ServeC] Sending successes connection message to client\n");
    write(resp_fd, "10", 2);

    client->req_fd = req_fd;
    client->resp_fd = resp_fd;
    client->notif_fd = notif_fd;
    
    while (1){
      printf("[ServeC] Waiting for a command(request) from client\n");

      //m get commands
      char op;
      int value = (int)read(req_fd, &op, 1);
      if (value == 0){// client closed req_fd 
        break;
      } else if(value == -1){
        fprintf(stderr, "Error reading commands from client\n");
      }

      if (op == '2') {
        printf("[ServeC] Entering diconnect_client\n");
        value = kvs_disconnect_client(client);
        if(value != 0){
          fprintf(stderr, "Error disconnecting the client from the kvs table\n");
        }
        printf("[ServeC] back in serve_client from diconnect_client\n");
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
    printf("[ServeC] closing fifos client sent\n");

    close(req_fd);
    close(resp_fd);
    close(notif_fd);
    
    printf("[ServeC] Finished with this client\n");
  }
}


int get_requests(char *regist_pipe_path){ //estava void
  printf("[GetReq] Entered get_requests\n");

  if (pthread_sigmask(SIG_UNBLOCK, &sigset_anf, NULL) != 0) {
      fprintf(stderr, "Error unblocking SIGUSR1 \n");
      return 1;
  }

  //m create and open requests
  int error = 0;
  //init semaphores
  printf("[GetReq] Inicializing semaphores\n");
  sem_init(&sem_prod, 0, MAX_SESSION_COUNT);
  sem_init(&sem_consum, 0, 0);

  //creating regist fifo
  //umask(0);
  printf("[GetReq] Creating regists fifo\n");
  if (mkfifo(regist_pipe_path, 0666) == -1) {
    perror("mkfifo failed");
    if (errno != EEXIST) {
      fprintf(stderr, "Failed to create regists thread\n");
      return 1;
    }
    printf("ERROR\n");
  }
  printf("[GetReq] Regist fifo created successfully\n");

  // opening regist fifo
  printf("[GetReq] Opening regist fifo\n");
  int regist_fd = open(regist_pipe_path, O_RDONLY);
  while (errno == EINTR){
    errno = 0;
    regist_fd = open(regist_pipe_path, O_RDONLY);
  }
  if (regist_fd == -1) {
    fprintf(stderr,"Failed to open fifo <%s> for reading\n", regist_pipe_path);
    unlink(regist_pipe_path);
    return 1;
  }
  printf("[GetReq] Regist fifo opened successfully\n");

  printf("[GetReq] Creating threads to manage clients\n");
  int thread_id;
  //sending client handling threads
  pthread_t threads[MAX_SESSION_COUNT];
  for (size_t i = 0; i < MAX_SESSION_COUNT; i++) {
    if (pthread_create(&threads[i], NULL, serve_client, NULL) != 0) {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      error = 1;
      break;
    }
    thread_id = (int)i;
    printf("[GetReq] Thread [%d] starting\n", thread_id);
  }
  printf("[GetReq] %d threads created\n", thread_id + 1);
  
  //getting requests
  
  while (1){
    int intr = 0; //not used for now
    char buffer[1 + 40 + 40 + 40 + 1] = {'\0'}; //for clients request info
    printf("[GetReq] Waiting for a regist request\n");
    int value = (int)read_all(regist_fd, buffer, 1 + 40 + 40 + 40, &intr);
    if(intr == 1){continue;}
    else if(value == -1){
      fprintf(stderr,"Error, while reading request.\n");
    }else if(value == 0){
      printf("[GetReq] Client jclosed regist pipe \n");
      continue;
    }
    buffer[121] = '\0';
    printf("[GetReq] Regist request received\n");

    //wait for space to add new request
    printf("[GetReq] In production semaphore\n");
    sem_wait(&sem_prod);
    pthread_mutex_lock(&sem_lock);
    printf("[GetReq] Blocking prod-consum mutex\n");

    
    //int value = read_all(regist_fd, buffer, 1 + 40 + 40 + 40, &intr);
    
    printf("[GetReq] Printing paths in buffer\n");
    char path[41];
    printf("[GetReq] buffer req = <%s>\n", buffer);
    strncpy(path, buffer + 41, 41);
    printf("[GetReq] buffer resp = <%s>\n", path);
    strncpy(path, buffer + 81, 41);
    printf("[GetReq] buffer notif = <%s>\n", path);
  

    //adding to prod-consum buffer
    //char *dup = strdup(buffer);
    
    printf("[GetReq] Adding a client request to buffer of prod-consum\n");
    printf("[GetReq] Indice of production = %d at the moment\n", i_prod);
    memcpy(buffer_p_c[i_prod], buffer, 1 + 40 + 40 + 40 + 1);

    printf("[GetReq] Confirming paths in prod-consum buffer\n");
    printf("[GetReq] buff_p_c req= <%s>\n", buffer_p_c[i_prod]);
    strncpy(path, buffer_p_c[i_prod] + 41, 41);
    printf("[GetReq] buff_p_c resp= <%s>\n", path);
    strncpy(path, buffer_p_c[i_prod] + 81, 41);
    printf("[GetReq] buff_p_c notif= <%s>\n", path);

    
    i_prod = (i_prod + 1)%MAX_SESSION_COUNT;
    printf("[GetReq] Indice of production = %d atualized\n",i_prod);

    
    

    printf("[GetReq] Unlocking prod-consum mutex\n");
    pthread_mutex_unlock(&sem_lock);
    sem_post(&sem_consum); //new client to attend
    printf("[GetReq] 1 element added to consumer semaphore\n");
  
    printf("[GetReq]wait for new client request\n");
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
  printf("[DpThreads] Entered dispatch_hreads\n");
  pthread_t *threads = malloc(max_threads * sizeof(pthread_t));

  if (threads == NULL) {
    fprintf(stderr, "Failed to allocate memory for threads\n");
    return;
  }

  struct SharedData thread_data = {dir, jobs_directory, PTHREAD_MUTEX_INITIALIZER};

  printf("[DpThreads] Creating Jobs Threads\n");
  for (size_t i = 0; i < max_threads; i++) {
    if (pthread_create(&threads[i], NULL, get_file, (void *)&thread_data) != 0) {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return; // Se a thread não for criada, pulamos para o próximo cliente
    }
  }

  printf("[DpThreads] Jobs threads created\n");
  // ler do FIFO de registo
  printf("[DpThreads] Entering get_requests\n");
  get_requests(regist_path);
  printf("[DpThreads] Back to dispactch_threads from get_requests\n");



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
  // SIGNALS
  printf("[Main] Entered Main\n");

  struct sigaction sa;

  // Set up the SIGUSR1 handler
  sa.sa_handler = sigusr1_handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask); // No additional signals to block in the handler
  if (sigaction(SIGUSR1, &sa, NULL) == -1) {
      fprintf(stderr, "Error, couldn't set the handler for SIGUSR1\n");
      return 1;
  }

  sigemptyset(&sigset_anf);
  sigaddset(&sigset_anf, SIGUSR1);
  if (pthread_sigmask(SIG_BLOCK, &sigset_anf, NULL) != 0) {
      perror("pthread_sigmask - block");
      return 1;
  }
  //TEMPORARY HANDLER FOR ctrl-z
  //signal(SIGTSTP, sigtstp_handler);

  //END SIGNALS
  if (argc < 4) {
    write_str(STDERR_FILENO, "Usage: ");
    write_str(STDERR_FILENO, argv[0]);
    write_str(STDERR_FILENO, " <jobs_dir>");
    write_str(STDERR_FILENO, " <max_threads>");
    write_str(STDERR_FILENO, " <max_backups> \n");
    return 1;
  }

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

  printf("[Main] Entering Dispach Threads\n");
  dispatch_threads(dir, argv[4]);
  printf("[Main] Back to Main from Dispatch Threads\n");


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
