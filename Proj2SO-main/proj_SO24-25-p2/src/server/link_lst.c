#include "link_lst.h"

Node *create_node(int data) {
    Node *new_node = (Node *)malloc(sizeof(Node));
    if (new_node == NULL) {
        fprintf(stderr, "Erro ao alocar memória");
        exit(EXIT_FAILURE);
    }
    new_node->data = data;
    new_node->next = NULL;
    return new_node;
}

void append_node(Node **head, int data) {
    Node *new_node = create_node(data);
    if (*head == NULL) {
        *head = new_node;
        return;
    }
    Node *current = *head;
    while (current->next != NULL) {
        current = current->next;
    }
    current->next = new_node;
}

void free_list(Node *head) {
    Node *current = head;
    Node *next;
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }
}

int remove_node(Node **head, int value) {
    if (*head == NULL) {
        fprintf(stderr,"A lista está vazia. Nada para remover.\n");
        return 0;
    }

    Node *current = *head;
    Node *prev = NULL;

    if (current->data == value) {
        *head = current->next;
        free(current);
        return 1;
    }

    while (current != NULL) {
        if (current->data == value){
            break;
        }
        prev = current;
        current = current->next;
    }
    if (current == NULL) {
        return 0;
    }

    prev->next = current->next;
    free(current);
    return 1;
}

void print_int_list(Node *head){
    printf("Elements = [");
    while (head != NULL) {
        printf("%d,",head->data);
        head = head->next;
    }
    printf("]\n");
}
