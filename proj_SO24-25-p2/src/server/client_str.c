#include "client_str.h"


void client_init(Client *client){
    client->n_keys = 0;
    client->req_fd = 0;
    client->resp_fd = 0;
    client->notif_fd = 0;
    client->keys = NULL;
}

// Function to create a new node
Node_str *create_node_str(const char *string) {
    Node_str *new_node = (Node_str *)malloc(sizeof(Node_str));
    if (new_node == NULL) {
        perror("Error allocating memory");
        exit(1);
    }
    strncpy(new_node->str, string, MAX_STRING_SIZE);
    new_node->str[MAX_STRING_SIZE] = '\0'; // Ensure the string is null-terminated
    new_node->next = NULL;
    return new_node;
}

// Function to append a node to the end of the list
void append_node_str(Node_str **head, const char *string) {
    Node_str *new_node = create_node_str(string);

    if (*head == NULL) {
        *head = new_node;
    } else {
        Node_str *current = *head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }
}

// Function to remove a node by string value
int remove_node_str(Node_str **head, const char *string) {
    if (*head == NULL) {
        return 0; // List is empty, nothing to remove
    }

    Node_str *current = *head;
    Node_str *previous = NULL;

    // Traverse the list to find the matching node
    while (current != NULL) {
        if (strcmp(current->str, string) == 0) {
            // Match found
            if (previous == NULL) {
                // Removing the head node
                *head = current->next;
            } else {
                // Removing a node in the middle or end
                previous->next = current->next;
            }
            free(current);
            return 1; // Element successfully removed
        }
        previous = current;
        current = current->next;
    }

    return 0; // Element not found
}

// Function to free the memory of all nodes in the list
void destroy_str_list(Node_str *head) {
    Node_str *current = head;
    while (current != NULL) {
        Node_str *next = current->next;
        free(current);
        current = next;
    }
}

void print_str_list(Node_str *head){
    printf("Elements = [");
    while (head != NULL) {

        printf("%s,",head->str);
        head = head->next;
    }
    printf("]\n");
}