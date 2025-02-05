#include "operations.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/common/io.h"

#include "client_str.h"
#include "io.h"
#include "kvs.h"


static struct HashTable *kvs_table = NULL;

Client *clients = NULL;

/// Calculates a timespec from a delay in milliseconds.
/// @param delay_ms Delay in milliseconds.
/// @return Timespec with the given delay.
static struct timespec delay_to_timespec(unsigned int delay_ms) {
  return (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
}

void sigusr1_handler(){
  remove_all_clients();
}

void kvs_remove_client(int id){
  remove_client(&clients, id);
}


void remove_all_clients(){
  Client *head = clients;
  clients = NULL;

  while(head != NULL){
    close(head->req_fd);
    close(head->resp_fd);
    close(head->notif_fd);
    Node_str *ks = head->keys;
    while(ks != NULL){
      unsubscribe_pair(kvs_table, ks->str, head->id);
      ks = ks->next;
    }
    destroy_client(head);
    head = head->next;
  }
}

int kvs_init() {
  if (kvs_table != NULL) {
    fprintf(stderr, "KVS state has already been initialized\n");
    return 1;
  }

  kvs_table = create_hash_table();
  return kvs_table == NULL;
}

int kvs_terminate() {
  if (kvs_table == NULL) {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  free_table(kvs_table);
  kvs_table = NULL;
  return 0;
}

int kvs_write(size_t num_pairs, char keys[][MAX_STRING_SIZE],
              char values[][MAX_STRING_SIZE]) {
  if (kvs_table == NULL) {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  pthread_rwlock_wrlock(&kvs_table->tablelock);

  for (size_t i = 0; i < num_pairs; i++) {
    if (write_pair(kvs_table, keys[i], values[i]) != 0) {
      fprintf(stderr, "Failed to write key pair (%s,%s)\n", keys[i], values[i]);
    }

    //m notify client
    Node *ids = NULL;
    get_clients_ids(kvs_table, keys[i], &ids);
    while(ids != NULL){
      Client *client = get_client(clients, ids->data);
      if(client == NULL){
        fprintf(stderr, "Error, for some reason the client with the given id doesnt exist\n");
        return 1;
      }
      char buffer[41 + 41];
      strncpy(buffer, keys[i], 41);
      strncpy(buffer + 41, values[i], 41);
      write_all(client->notif_fd, buffer,41 + 41);
      ids = ids->next;
    }

  }

  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0;
}

int kvs_read(size_t num_pairs, char keys[][MAX_STRING_SIZE], int fd) {
  if (kvs_table == NULL) {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  pthread_rwlock_rdlock(&kvs_table->tablelock);

  write_str(fd, "[");
  for (size_t i = 0; i < num_pairs; i++) {
    char *result = read_pair(kvs_table, keys[i]);
    char aux[MAX_STRING_SIZE];
    if (result == NULL) {
      snprintf(aux, MAX_STRING_SIZE, "(%s,KVSERROR)", keys[i]);
    } else {
      snprintf(aux, MAX_STRING_SIZE, "(%s,%s)", keys[i], result);
    }
    write_str(fd, aux);
    free(result);
  }
  write_str(fd, "]\n");

  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0;
}

int kvs_delete(size_t num_pairs, char keys[][MAX_STRING_SIZE], int fd) {
  if (kvs_table == NULL) {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }
  pthread_rwlock_wrlock(&kvs_table->tablelock);

  int aux = 0;
  for (size_t i = 0; i < num_pairs; i++) {
    
    Node *ids = NULL;
    get_clients_ids(kvs_table, keys[i], &ids);
    while(ids != NULL){
      Client *client = get_client(clients, ids->data);
      if(client == NULL){
        fprintf(stderr, "Error, for some reason the client with the given id doesnt exist\n");
        return 1;
      }

      //notify client
      char buffer[41 + 41];
      strncpy(buffer, keys[i], 41);
      strncpy(buffer + 41, "DELETED", 41);
      write_all(client->notif_fd, buffer,41 + 41);

      //remove key from client subsscribed keys
      remove_node_str(&(client->keys), keys[i]);
      client->n_keys--;

      ids = ids->next;

    }
    
    if (delete_pair(kvs_table, keys[i]) != 0) {
      if (!aux) {
        write_str(fd, "[");
        aux = 1;
      }
      char str[MAX_STRING_SIZE];
      snprintf(str, MAX_STRING_SIZE, "(%s,KVSMISSING)", keys[i]);
      write_str(fd, str);
    }

  }
  if (aux) {
    write_str(fd, "]\n");
  }

  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0;
}

void kvs_show(int fd) {
  if (kvs_table == NULL) {
    fprintf(stderr, "KVS state must be initialized\n");
    return;
  }

  pthread_rwlock_rdlock(&kvs_table->tablelock);
  char aux[MAX_STRING_SIZE];

  for (int i = 0; i < TABLE_SIZE; i++) {
    KeyNode *keyNode = kvs_table->table[i]; // Get the next list head
    while (keyNode != NULL) {
      snprintf(aux, MAX_STRING_SIZE, "(%s, %s)\n", keyNode->key,
               keyNode->value);
      write_str(fd, aux);
      keyNode = keyNode->next; // Move to the next node of the list
    }
  }

  pthread_rwlock_unlock(&kvs_table->tablelock);
}

int kvs_backup(size_t num_backup, char *job_filename, char *directory) {
  pid_t pid;
  char bck_name[50];
  snprintf(bck_name, sizeof(bck_name), "%s/%s-%ld.bck", directory,
           strtok(job_filename, "."), num_backup);

  pthread_rwlock_rdlock(&kvs_table->tablelock);
  pid = fork();
  pthread_rwlock_unlock(&kvs_table->tablelock);
  if (pid == 0) {
    // functions used here have to be async signal safe, since this
    // fork happens in a multi thread context (see man fork)
    int fd = open(bck_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    for (int i = 0; i < TABLE_SIZE; i++) {
      KeyNode *keyNode = kvs_table->table[i]; // Get the next list head
      while (keyNode != NULL) {
        char aux[MAX_STRING_SIZE];
        aux[0] = '(';
        size_t num_bytes_copied = 1; // the "("
        // the - 1 are all to leave space for the '/0'
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, keyNode->key,
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, ", ",
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, keyNode->value,
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, ")\n",
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        aux[num_bytes_copied] = '\0';
        write_str(fd, aux);
        keyNode = keyNode->next; // Move to the next node of the list
      }
    }
    exit(1);
  } else if (pid < 0) {
    return -1;
  }
  return 0;
}



void kvs_wait(unsigned int delay_ms) {
  struct timespec delay = delay_to_timespec(delay_ms);
  nanosleep(&delay, NULL);
}



int kvs_disconnect_client(Client *client){
  if(client == NULL){
    return 1;
  }
  
  Node_str *keys = (client->keys);
  while(keys != NULL){
    unsubscribe_pair(kvs_table, keys->str, client->id);
    keys = keys->next;
  }

  
  write(client->resp_fd, "20",2);

  //removes the client from the list fo clients
  remove_client(&clients, client->id);

  return 0;
}



//still need to do the case if the key is already subscribed
int kvs_subscribe_key(Client *client){ 
  if (client == NULL){return 1;}
  
  char key[41] = {'\0'};
  int value = (int)read(client->req_fd, key, 40);
  
  if (errno == EBADF){
    return 2; //get a new client
  } else if (value == -1){
    fprintf(stderr,"Error while reading the key for subscription\n");
    return 1;
  } else if(value == 0){
    return 2;// get a new client
  }

  
  
  //result only depends whether the key exists on not 1 or 0 respectively
  int result = 0;
  if (find_pair(kvs_table, key)){
    result = 1;
    if (client->n_keys < MAX_NUMBER_SUB && find_node_str(client->keys, key) != 1){ 
      //things in client
      append_node_str(&(client->keys), key);
      client->n_keys++;
      //things in kvs table
      subscribe_pair(kvs_table, key, client->id);
   }
  }
  
  
  //m print message with result
  write(client->resp_fd, "3",1);
  if (errno == EBADF){return 2;}

  char c = (char)(result+ 48); //na ASCII '0' = 48
  
  write(client->resp_fd, &c, 1);  
  if (errno == EBADF){return 2;}

  return 0;
}

int kvs_unsubscribe_key(Client *client){
  if (client == NULL){return 1;}
  char key[41] = {'\0'};
  int value = (int)read(client->req_fd, key, 40);
  if (errno == EBADF){
    return 2; //get a new client
  } else if (value == -1 || value == 0){
    fprintf(stderr,"Error while reading the key for unsubscription\n");
    return 1;
  }
  else if(value == 0){
    return 2;
  }
  

  int result = 1; //0 if was subscribed, 1 if wasnt
  if (find_node_str(client->keys, key) == 1){ //if subscribed
    result = 0;
    //remove from client list
    remove_node_str(&(client->keys), key);
    client->n_keys--;
    //remove from kvs
    unsubscribe_pair(kvs_table, key, client->id);
  }
  //m print message with result
  write(client->resp_fd, "4",1);
  if (errno == EBADF){return 2;}
  char c = (char)(result + 48);//na ASCII '0' = 48
  value = (int)write(client->resp_fd, &c, 1);
  if (errno == EBADF){return 2;}

  return 0;
}


void add_Client(Client *client){
  append_client(&clients, client);
}
