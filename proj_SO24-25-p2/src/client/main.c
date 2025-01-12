#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parser.h"
#include "src/client/api.h"
#include "src/common/constants.h"
#include "src/common/io.h"
#include "src/server/io.h"

static void *track_notif(void *arg){
  int notif_fd = *(int*)arg;
  char key[41], value[41];
  int intr;

  while (1){
    int result = 0;
    result = read_all(notif_fd, key, 41, &intr);
    if (result == 0){break;}
    else if (result == -1){
      fprintf(stderr, "Error with read all while reading notifications\n");
    }
    else if (intr == 1){
      fprintf(stderr, "Reading in the notification was inturupted\n");
    }

    result = read_all(notif_fd, value, 41, &intr);
    if(result == 0 || result == -1){
      printf("Error while reading from notifications\n");
    }
    else if (intr == 1){
      fprintf(stderr, "Reading in the notification was inturupted\n");
    }

    write_str(STDOUT_FILENO, "(");
    write_str(STDOUT_FILENO, key);
    write_str(STDOUT_FILENO, ",");
    write_str(STDOUT_FILENO, value);
    write_str(STDOUT_FILENO, ")\n");
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <client_unique_id> <register_pipe_path>\n",
            argv[0]);
    return 1;
  }
  char *server_pipe_path = argv[1];

  char req_pipe_path[256] = "/tmp/req";
  char resp_pipe_path[256] = "/tmp/resp";
  char notif_pipe_path[256] = "/tmp/notif";
  int req_fd = 0, resp_fd = 0, notif_fd = 0; //não estão a ser usados pq?

  char keys[MAX_NUMBER_SUB][MAX_STRING_SIZE] = {0};
  unsigned int delay_ms;
  size_t num;

  strncat(req_pipe_path, argv[1], strlen(argv[1]) * sizeof(char));
  strncat(resp_pipe_path, argv[1], strlen(argv[1]) * sizeof(char));
  strncat(notif_pipe_path, argv[1], strlen(argv[1]) * sizeof(char));

  
  // TODO open pipes
  
  
  int notif_pipe;
  if (kvs_connect(req_pipe_path, resp_pipe_path, server_pipe_path,
      notif_pipe_path, &notif_pipe) != 0){
    fprintf(stderr, "Failed to connect to server\n");
    return 1;
  }

  //m still have to create threads to do the operations that
  //m are responsible for receiving the notifications and sending
  //m it to the stdout: I thing notif_pipe is for here
  //m thread_func
  pthread_t notif_thread;
  if (pthread_create(&notif_thread, NULL, track_notif, (void *)&notif_pipe) !=
      0) {
    fprintf(stderr, "Failed to create notifications thread\n");
  }

  //m acho que a tarefa principal deverá ficar a realizar este
  //m while, tmb deve fazer os respetivos pedidos ao server
  //m e receber as respetivas respostas
  while (1) {

    switch (get_next(STDIN_FILENO)) {
    case CMD_DISCONNECT:
      if (kvs_disconnect() != 0) {
        fprintf(stderr, "Failed to disconnect to the server\n");
        return 1;
      }
      // TODO: end notifications thread
      //m lets put the join i think
      if (pthread_join(notif_thread, NULL) != 0) {
        fprintf(stderr, "Failed to join notification thread\n");
      }
      printf("Disconnected from server\n");
      return 0;

    case CMD_SUBSCRIBE:
      num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
      if (num == 0) {
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_subscribe(keys[0])) {
        fprintf(stderr, "Command subscribe failed\n");
      }

      break;

    case CMD_UNSUBSCRIBE:
      num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
      if (num == 0) {
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_unsubscribe(keys[0])) {
        fprintf(stderr, "Command subscribe failed\n");
      }

      break;

    case CMD_DELAY:
      if (parse_delay(STDIN_FILENO, &delay_ms) == -1) {
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay_ms > 0) {
        printf("Waiting...\n");
        delay(delay_ms);
      }
      break;

    case CMD_INVALID:
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;

    case CMD_EMPTY:
      break;

    case EOC:
      // input should end in a disconnect, or it will loop here forever
      break;
    }
  }
}
