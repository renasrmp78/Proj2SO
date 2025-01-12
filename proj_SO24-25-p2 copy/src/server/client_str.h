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
}Node_str;

typedef struct Client{
    int req_fd, resp_fd, notif_fd, n_keys;
    Node_str *keys;

} Client;


void client_init(Client *client);

// Function to create a new node
Node_str *create_node_str(const char *string);

// Function to append a node to the end of the list
void append_node_str(Node_str **head, const char *string);

// Function to remove a node by string value
int remove_node_str(Node_str **head, const char *string);

// Function to free the memory of all nodes in the list
void destroy_str_list(Node_str *head);


#endif