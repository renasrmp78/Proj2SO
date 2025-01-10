#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "api.h"

#include "src/common/constants.h"
#include "src/common/protocol.h"
#include "src/common/io.h"
#include "src/server/io.h"

char const *req_pipe_path_c;
char const *resp_pipe_path_c;
char const *server_pipe_path_c;
char const *notif_pipe_path_c;

int req_fd;
int resp_fd;
int notif_fd;
int server_fd;

const char* OP_to_string(int op) {
    switch (op) {
        case OP_CODE_CONNECT: return "connect";
        case OP_CODE_DISCONNECT: return "disconnect";
        case OP_CODE_SUBSCRIBE: return "subscribe";
        case OP_COPE_UNSUBSCRIBE: return "unsubscribe";
        default: return "UNKNOWN";
    }
}

//m “Server returned <response-code> for operation: <connect|disconnect|subscribe|unsubscribe>
void print_answer(char ans_code, char op){
  write_str(STDOUT_FILENO, "Server returned ");
  write_str(STDOUT_FILENO, ans_code);
  write_str(STDOUT_FILENO, " for operation: ");
  write_str(STDOUT_FILENO, OP_to_string((int)op));
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
  if(mkfifo(req_pipe_path, 0666) != 0){
    fprintf("Failed to create fifo <%s>\n", req_pipe_path);
    return 1;
  }
  if(mkfifo(resp_pipe_path, 0666) != 0){
    fprintf("Failed to create fifo <%s>\n", resp_pipe_path);
    return 1;
  }
  if(mkfifo(notif_pipe_path, 0666) != 0){
    fprintf("Failed to create fifo <%s>\n", notif_pipe_path);
    return 1;
  }

  //m open connections
  //m connect to server
  server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    fprintf("Failed to open fifo <%s> for writing\n", server_pipe_path);
    return 1;
  }
  //m We now probably need to send to the server the name of the fifos we just created             !!!
  char buffer[1 + 40 + 40 + 40] = {'\0'};
  
  strncpy(buffer, "1", 1);

  char path[40] = '\0';
  strncpy(path, req_pipe_path, strlen(req_pipe_path));
  strncpy(buffer + 1, path, 40);

  memset(path, '\0', 40);
  strncpy(path, resp_pipe_path, strlen(resp_pipe_path));
  strncpy(buffer + 1 + 40, path, 40);

  memset(path, '\0', 40);
  strncpy(path, notif_pipe_path, strlen(notif_pipe_path));
  strncpy(buffer + 1 + 40 + 40, path, 40);

  write_all(server_fd, buffer, 1 + 40 + 40 + 40);
  //m Until the server connects to the respective fifos we will send, our program will
  //m bbe blocked in these next opens

  //m connect to requests pipe
  req_fd = open(req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    fprintf("Failed to open fifo <%s> for writing\n", req_pipe_path);
    return 1;
  }
  
  //m connect to answers pipe
  resp_fd = open(resp_pipe_path, O_RDONLY);
  if (resp_fd == -1) {
    fprintf("Failed to open fifo <%s> for reading\n", resp_pipe_path);
    return 1;
  }
  
  //m connect to notifications pipe
  notif_fd = open(notif_pipe_path, O_RDONLY);
  if (notif_fd == -1) {
    fprintf("Failed to open fifo <%s> for reading\n", notif_pipe_path);
    return 1;
  }
  *notif_pipe = notif_fd; //m secalhar vai ser necessario
  
  char buff[2];
  read_all(resp_fd, buff, 2, NULL);
  if (buff[0] != '1' || buff[1] != 0){
    fprintf(stderr, "Problem with server feedback about connecting\n");
    return 1;
  }

  print_answer(buff[1], (int)buff[0]);

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
  write_str(req_fd ,"2");
  char buff[2];
  read_all(resp_fd, buff, 2, NULL);

  if (buff[0] != '2' || buff[1] != 0){
    fprintf(stderr, "Problem with server feedback about desconnecting\n");
    return 1;
  }


  close(server_fd);
  close(req_fd);
  close(resp_fd);
  close(notif_fd);

  if(unlink(req_pipe_path_c) != 0){
    fprintf("Failed to destroy fifo <%s>\n", req_pipe_path_c);
    return 1;
  }

  if(unlink(resp_pipe_path_c) != 0){
    fprintf("Failed to destroy fifo <%s>\n", resp_pipe_path_c);
    return 1;
  }

  if(unlink(notif_pipe_path_c) != 0){
    fprintf("Failed to destroy fifo <%s>\n", notif_pipe_path_c);
    return 1;
  }

  return 0;

}

/**
 * @returns 0 if the doesnt exist, 1 if existed 
 */
int kvs_subscribe(const char *key) {
  // send subscribe message to request pipe and wait for response in response
  //m lets try
  write_str(req_fd, "3");
  write_all(req_fd ,key, 41);
  char buff[2];
  read_all(resp_fd, buff, 2, NULL);

  

  if (buff[0] != '3'){
    fprintf(stderr, "Problem with server feedback about subscribing key\n");
    return 1;
  }
  // buf[1] = 0 se chave não existia na hash, 1 se existia
  if (buff[1] == '0'){
    fprintf(stderr,"key didn't exist in server hash\n");
  }
  else if(buff[1] == '1'){ return 0;}
  
  return 1;
}

/**
 * @returns 0 if the subsctiption existed and wassuccesfuly removed, 1 case it didnt exist 
 */
int kvs_unsubscribe(const char *key) {
  // send unsubscribe message to request pipe and wait for response in response
  // pipe
  return 0;
}
