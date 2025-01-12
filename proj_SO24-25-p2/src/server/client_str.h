#ifndef CLIENT_STR_H
#define CLIENT_STR_H
#include "client_str.h"
#include "constants.h"

#include <ctype.h>
#include <stdlib.h>

#include "string.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct Node_str{
    char str[MAX_STRING_SIZE + 1];
    struct Node_str *next;
} Node_str;

typedef struct Client{
    int id, req_fd, resp_fd, notif_fd, n_keys; //= 0
    Node_str *keys; //=NULL
    struct Client *next;

} Client;

/** 
 * This function creates a client, and returns its pointer
*/
Client *create_client();

/**
 * This fucntions appents the client to the linked list
 */
void append_client(Client **head, Client *client);

/**
 * This function gets the client if exists, else
 * returns NULL
 */
Client *get_client(Client *head, int id);

/**
 * This functions removes client from the linked list and destroys it
 * @returns 1 if the client existd, 0 otherwise
 */
int remove_client(Client **head, int id);
/** 
 * This function destroys the client and its things
 */
void destroy_client(Client *client);

// Function to create a new node
Node_str *create_node_str(const char *string);

// Function to append a node to the end of the list
void append_node_str(Node_str **head, const char *string);

/** 
 * This function removes a node from the Node_str link list
 * 
 * @return 1 if element existed
 * @return 0 else
 */
int remove_node_str(Node_str **head, const char *string);

/**
 * @returns 1 if found, 0 otherwise
 */
int find_node_str(Node_str *head, const char *value);

// Function to free the memory of all nodes in the list
void destroy_str_list(Node_str *head);

void print_str_list(Node_str *head);

#endif