#include "kvs.h"

#include <ctype.h>
#include <stdlib.h>

#include "string.h"

// Hash function based on key initial.
// @param key Lowercase alphabetical string.
// @return hash.
// NOTE: This is not an ideal hash function, but is useful for test purposes of
// the project
int hash(const char *key) {
  int firstLetter = tolower(key[0]);
  if (firstLetter >= 'a' && firstLetter <= 'z') {
    return firstLetter - 'a';
  } else if (firstLetter >= '0' && firstLetter <= '9') {
    return firstLetter - '0';
  }
  return -1; // Invalid index for non-alphabetic or number strings
}

struct HashTable *create_hash_table() {
  HashTable *ht = malloc(sizeof(HashTable));
  if (!ht)
    return NULL;
  for (int i = 0; i < TABLE_SIZE; i++) {
    ht->table[i] = NULL;
  }
  pthread_rwlock_init(&ht->tablelock, NULL);
  return ht;
}

int write_pair(HashTable *ht, const char *key, const char *value) {
  int index = hash(key);

  // Search for the key node
  KeyNode *keyNode = ht->table[index];
  KeyNode *previousNode;

  while (keyNode != NULL) {
    if (strcmp(keyNode->key, key) == 0) {
      // overwrite value
      free(keyNode->value);
      keyNode->value = strdup(value);
      return 0;
    }
    previousNode = keyNode;
    keyNode = previousNode->next; // Move to the next node
  }
  // Key not found, create a new key node
  keyNode = malloc(sizeof(KeyNode));
  keyNode->key = strdup(key);       // Allocate memory for the key
  keyNode->value = strdup(value);   // Allocate memory for the value
  keyNode->next = ht->table[index]; // Link to existing nodes
  ht->table[index] = keyNode; // Place new key node at the start of the list
  return 0;
}

char *read_pair(HashTable *ht, const char *key) {
  int index = hash(key);

  KeyNode *keyNode = ht->table[index];
  KeyNode *previousNode;
  char *value;

  while (keyNode != NULL) {
    if (strcmp(keyNode->key, key) == 0) {
      value = strdup(keyNode->value);
      return value; // Return the value if found
    }
    previousNode = keyNode;
    keyNode = previousNode->next; // Move to the next node
  }

  return NULL; // Key not found
}

int delete_pair(HashTable *ht, const char *key) {
  int index = hash(key);

  // Search for the key node
  KeyNode *keyNode = ht->table[index];
  KeyNode *prevNode = NULL;

  while (keyNode != NULL) {
    if (strcmp(keyNode->key, key) == 0) {
      // Key found; delete this node
      if (prevNode == NULL) {
        // Node to delete is the first node in the list
        ht->table[index] =
            keyNode->next; // Update the table to point to the next node
      } else {
        // Node to delete is not the first; bypass it
        prevNode->next =
            keyNode->next; // Link the previous node to the next node
      }
      // Free the memory allocated for the key and value
      free(keyNode->key);
      free(keyNode->value);
      free_list(keyNode->ids);
      free(keyNode); // Free the key node itself
      return 0;      // Exit the function
    }
    prevNode = keyNode;      // Move prevNode to current node
    keyNode = keyNode->next; // Move to the next node
  }

  return 1;
}


int subscribe_pair(HashTable *ht, const char *key, int id){
  int index = hash(key);
  
  KeyNode *keyNode = ht->table[index];

  while(keyNode != NULL){
    if(strcmp(keyNode->key, key) == 0){
      print_int_list(keyNode->ids);
      append_node(&(keyNode->ids), id); //m not sure
      print_int_list(keyNode->ids);
      return 1;
    }
    keyNode = keyNode->next;
  }

  return 0;
}

int unsubscribe_pair(HashTable *ht, const char *key, int id){
  int index = hash(key);
  
  KeyNode *keyNode = ht->table[index];

  while(keyNode != NULL){ //percorre ate encontrar a key certa
    if(strcmp(keyNode->key, key) == 0){
      printf("in right key\n");
      print_int_list(keyNode->ids);
      if(remove_node(&(keyNode->ids), id) == 1){ //m if existed
        printf("here1\n");
        print_int_list(keyNode->ids);
        return 0;
      } else{
        printf("here2\n");
        print_int_list(keyNode->ids);
      }
      printf("really ?? breaking\n");
      break; //every key are unique
    }
    keyNode = keyNode->next;
  }

  return 1;
}

int find_pair(HashTable *ht, const char *key){
  int index = hash(key);

  KeyNode *head = ht->table[index];
  while (head != NULL){
    if (strcmp(head->key, key) == 0){ //found
      return 1;
    }
    head = head->next;
  }
  return 0; //not found
}

void get_clients_ids(HashTable *ht, char *key, Node **lk_lst){
  
  printf("key = <%s>", key);
  printf("c1\n");
  int index = hash(key);
  printf("<%d>", index);
  printf("c2\n");
  if(ht == NULL){printf("hell nah\n");}
  if((ht->table)[index] == NULL){printf("hell nah2\n");}
  *lk_lst = (((ht->table)[index])->ids);
  printf("c3\n");
}



void free_table(HashTable *ht) {
  for (int i = 0; i < TABLE_SIZE; i++) {
    KeyNode *keyNode = ht->table[i];
    while (keyNode != NULL) {
      KeyNode *temp = keyNode;
      keyNode = keyNode->next;
      free(temp->key);
      free(temp->value);
      free_list(temp->ids);
      free(temp);
    }
  }
  pthread_rwlock_destroy(&ht->tablelock);
  free(ht);
}
