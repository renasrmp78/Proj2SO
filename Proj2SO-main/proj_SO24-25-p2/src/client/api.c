#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#include "api.h"

#include "src/common/constants.h"
#include "src/common/protocol.h"
#include "src/common/io.h"
#include "src/server/io.h"

#include <errno.h>
#include <sys/stat.h>


char const *req_pipe_path_c;
char const *resp_pipe_path_c;
char const *server_pipe_path_c;
char const *notif_pipe_path_c;

int req_fd;
int resp_fd;
int notif_fd;
int server_fd;

void sigtstp_handler() {
    // Clean up resources here
    if(unlink(req_pipe_path_c) != 0){
      fprintf(stderr,"Failed to destroy fifo <%s>\n", req_pipe_path_c);
    }

    if(unlink(resp_pipe_path_c) != 0){
      fprintf(stderr,"Failed to destroy fifo <%s>\n", resp_pipe_path_c);
    }

    if(unlink(notif_pipe_path_c) != 0){
      fprintf(stderr,"Failed to destroy fifo <%s>\n", notif_pipe_path_c);
    }
    exit(0);  // Terminate program after cleanup
}

const char* OP_to_string(int op) {
    switch (op) {
        case 1: return "connect";
        case OP_CODE_DISCONNECT: return "disconnect";
        case OP_CODE_SUBSCRIBE: return "subscribe";
        case OP_COPE_UNSUBSCRIBE: return "unsubscribe";
        default: return "UNKNOWN";
    }
}

//m “Server returned <response-code> for operation: <connect|disconnect|subscribe|unsubscribe>
void print_answer(char ans_code, char op){
  //char message[128];
  write_str(STDOUT_FILENO, "Server returned ");
  char str[2];
  str[0] = ans_code;
  str[1] = '\0';
  write_str(STDOUT_FILENO, str);
  write_str(STDOUT_FILENO, " for operation: ");
  write_str(STDOUT_FILENO, OP_to_string((int)op - 48)); // 48 is 0 in the ASCCI
  write_str(STDOUT_FILENO, "\n");
}


int kvs_connect(char const *req_pipe_path, char const *resp_pipe_path,
                char const *server_pipe_path, char const *notif_pipe_path,
                int *notif_pipe) {
  
  //m keep them in this file?
  req_pipe_path_c = req_pipe_path;
  resp_pipe_path_c = resp_pipe_path;
  server_pipe_path_c = server_pipe_path;
  notif_pipe_path_c = notif_pipe_path;
  
  // create pipes and connect
  //m now create them
  if(mkfifo(req_pipe_path, 0666)){
    if (errno != EEXIST) {
      fprintf(stderr, "Failed to create fifo <%s>\n", req_pipe_path);
      return 1;
    }
  }
  if(mkfifo(resp_pipe_path, 0666) != 0){
    if (errno != EEXIST) {
      fprintf(stderr, "Failed to create fifo <%s>\n", resp_pipe_path);
      return 1;
    }
  }
  if(mkfifo(notif_pipe_path, 0666) != 0){
    if (errno != EEXIST) {
      fprintf(stderr,"Failed to create fifo <%s>\n", notif_pipe_path);
      return 1;
    }
  }

  //m open connections
  //m connect to server
  server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    fprintf(stderr,"Failed to open fifo <%s> for writing\n", server_pipe_path);
    return 1;
  }
  //m We now probably need to send to the server the name of the fifos we just created             !!!
  char buffer[1 + 40 + 40 + 40 + 1];
  snprintf(buffer, 2, "1"); //he needs the secont char for this.

  //each time the '\0' char will be overwriten by the next path
  //strncpy(path, req_pipe_path, 41);
  strncpy(buffer + 1, req_pipe_path, 41);
  //strncpy(path, resp_pipe_path, 41);
  strncpy(buffer + 1 + 40, resp_pipe_path, 41);
  //strncpy(path, notif_pipe_path, 41);
  strncpy(buffer + 1 + 40 + 40, notif_pipe_path, 41);

  write_all(server_fd, buffer, 1 + 40 + 40 + 40);
  //m Until the server connects to the respective fifos we will send, our program will
  //m bbe blocked in these next opens

  //m connect to requests pipe
  req_fd = open(req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for writing\n", req_pipe_path);
    return 1;
  }

  
  //m connect to answers pipe
  resp_fd = open(resp_pipe_path, O_RDONLY);
  if (resp_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for reading\n", resp_pipe_path);
    return 1;
  }


  //m connect to notifications pipe
  notif_fd = open(notif_pipe_path, O_RDONLY);
  if (notif_fd == -1) {
    fprintf(stderr, "Failed to open fifo <%s> for reading\n", notif_pipe_path);
    return 1;
  }

  *notif_pipe = notif_fd; //m secalhar vai ser necessario
  char buff[3];
  read_all(resp_fd, buff, 2, NULL); //could ve been right after answer connection
  

  buff[2] = '\0';

  if (buff[0] != '1'){
    fprintf(stderr, "Problem with server feedback about connecting\n");
    return 1;
  }
  print_answer(buff[1], buff[0]);

  return 0;
}

/**m
 * Envia um pedido de disconnect para o FIFO de pedido
 * Feixa os named pipes que abriu no connect e 
 * apaga os named pipe que criou
 * 
 * O servidor deverá eliminar todas as subscricoes deste cliente
 */
int kvs_disconnect(void) {
  // close pipes and unlink pipe files
  
  //m I think, it needs to send a menssage to the server here                                      !!!

  //m nos exemplos do lab, não verificam erro neste tipo de close
  
  //m Communicate with server
  
  write(req_fd ,"2", 1);
  
  char buff[2];
  read(resp_fd, buff, 2);

  
  if (buff[0] != '2'){
    fprintf(stderr, "Problem with server feedback about desconnecting\n");
    return 1;
  }

  print_answer(buff[1], buff[0]);

  
  close(server_fd);
  close(req_fd);
  close(resp_fd);
  close(notif_fd);

  if(unlink(req_pipe_path_c) != 0){
    fprintf(stderr,"Failed to destroy fifo <%s>\n", req_pipe_path_c);
    return 1;
  }

  if(unlink(resp_pipe_path_c) != 0){
    fprintf(stderr,"Failed to destroy fifo <%s>\n", resp_pipe_path_c);
    return 1;
  }

  if(unlink(notif_pipe_path_c) != 0){
    fprintf(stderr,"Failed to destroy fifo <%s>\n", notif_pipe_path_c);
    return 1;
  }

  return 0;

}

/**
 * @returns 0 if the doesnt exist, 1 if existed 
 */
int kvs_subscribe(const char *key) {
  // send subscribe message to request pipe and wait for response in response
  //EPIPE
  //m lets try 
  if (write_all(req_fd, "3", 1) == -1 || write_all(req_fd ,key, 40) == -1){
    fprintf(stderr, "Error writing subscribtion request to server\n");
  }
  char buff[3];
  read_all(resp_fd, buff, 2, NULL);
  buff[2] = '\0';
  if (buff[0] != '3'){
    fprintf(stderr, "Problem with server feedback about subscribing key\n");
    return 1;
  }

  print_answer(buff[1], buff[0]);
  
  return 0;
}

/**
 * @returns 0 if the subsctiption existed and wassuccesfuly removed, 1 case it didnt exist 
 */
int kvs_unsubscribe(const char *key) {
  // send unsubscribe message to request pipe and wait for response in response
  // pipe

  //m lets try
  if (write_all(req_fd, "4", 1) == -1 || write_all(req_fd ,key, 40) == -1){
    if (errno == EPIPE){
      return 2;
    }
    fprintf(stderr, "Error writing unsubscribtion request to server\n");
  }

  char buff[2];
  if (read_all(resp_fd, buff, 2, NULL) == 0){
    return 2;
  }

  if (buff[0] != '4'){
    fprintf(stderr, "Problem with server feedback about subscribing key\n");
    return 1;
  }

  print_answer(buff[1], buff[0]);

  return 0;
}
