#ifndef LINK_LST_H
#define LINK_LST_H
#include <ctype.h>
#include <stdlib.h>

#include "string.h"

#include <stdio.h>
#include <stdlib.h>

// Definindo a estrutura de um nó
typedef struct Node {
    int data;            // Dado armazenado no nó
    struct Node *next;   // Ponteiro para o próximo nó
} Node;

// Função para criar um novo nó
Node *create_node(int data);

// Função para adicionar um nó no final da lista
void append_node(Node **head, int data);
// Função para liberar a memória de toda a lista
void free_list(Node *head);

// Função para remover um nó com um valor específico
// returns 1 if the value existex, else 0
int remove_node(Node **head, int value);

void print_int_list(Node *head);

#endif