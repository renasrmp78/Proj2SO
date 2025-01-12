#include "link_lst.h"

// Função para criar um novo nó
Node *create_node(int data) {
    Node *new_node = (Node *)malloc(sizeof(Node));  // Alocando memória para o nó
    if (new_node == NULL) {
        fprintf(stderr, "Erro ao alocar memória");
        exit(EXIT_FAILURE);
    }
    new_node->data = data;  // Armazenando o valor
    new_node->next = NULL;  // Próximo nó inicialmente é NULL
    return new_node;
}

// Função para adicionar um nó no final da lista
void append_node(Node **head, int data) {
    Node *new_node = create_node(data);
    if (*head == NULL) {  // Se a lista estiver vazia
        *head = new_node;
        return;
    }
    Node *current = *head;
    while (current->next != NULL) {  // Percorrer até o último nó
        current = current->next;
    }
    current->next = new_node;  // Apontar o último nó para o novo nó
}

// Função para liberar a memória de toda a lista
void free_list(Node *head) {
    Node *current = head;
    Node *next;
    while (current != NULL) {
        next = current->next;  // Salvar o próximo nó
        free(current);         // Liberar o nó atual
        current = next;        // Ir para o próximo nó
    }
}

// Função para remover um nó com um valor específico
int remove_node(Node **head, int value) {
    if (*head == NULL) {  // Lista vazia
        fprintf(stderr,"A lista está vazia. Nada para remover.\n");
        return 0;
    }

    Node *current = *head;
    Node *prev = NULL;

    // Se o nó a ser removido é o primeiro (head)
    if (current->data == value) {
        *head = current->next;  // Mover o ponteiro head para o próximo nó
        free(current);          // Liberar o nó antigo
        return 1;
    }

    // Percorrer a lista para encontrar o nó a ser removido
    while (current != NULL) {
        printf("current data = %d\n", current->data);
        printf("value= %d\n", value);
        if (current->data == value){
            break;
        }
        prev = current;          // Guardar o nó atual como o anterior
        current = current->next; // Avançar para o próximo nó
    }
    printf("really??333\n");
    if (current == NULL) {  // Valor não encontrado
        return 0;
    }

    // Remover o nó encontrado
    printf("really222??\n");
    prev->next = current->next;  // Fazer o nó anterior apontar para o próximo do nó atual
    free(current);               // Liberar o nó atual
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