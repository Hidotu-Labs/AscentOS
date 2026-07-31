#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* Simple linked list */
typedef struct node {
    int value;
    struct node *next;
} node_t;

static node_t *list_push(node_t *head, int value) {
    node_t *n = malloc(sizeof(node_t));
    if (!n) { perror("malloc"); exit(1); }
    n->value = value;
    n->next  = head;
    return n;
}

static void list_free(node_t *head) {
    while (head) {
        node_t *tmp = head->next;
        free(head);
        head = tmp;
    }
}

/* Recursive Fibonacci */
static long fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

/* Bubble sort */
static void bubble_sort(int *arr, int len) {
    for (int i = 0; i < len - 1; i++)
        for (int j = 0; j < len - 1 - i; j++)
            if (arr[j] > arr[j + 1]) {
                int tmp  = arr[j];
                arr[j]   = arr[j + 1];
                arr[j + 1] = tmp;
            }
}

/* Basic string operations */
static void string_demo(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "AscentOS gcc test — built %s", __DATE__);
    printf("  string : %s\n", buf);
    printf("  length : %zu\n", strlen(buf));

    char *dup = strdup(buf);
    for (char *p = dup; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    printf("  upper  : %s\n", dup);
    free(dup);
}

/* Float / math */
static void math_demo(void) {
    double x = 2.0;
    printf("  sqrt(%.1f) = %.6f\n", x, sqrt(x));
    printf("  pi        = %.10f\n", 4.0 * atan(1.0));
}

int main(void) {
    puts("=== AscentOS test.c ===");

    /* --- linked list --- */
    puts("[1] linked list");
    node_t *list = NULL;
    for (int i = 1; i <= 5; i++) list = list_push(list, i * 10);
    printf("  list: ");
    for (node_t *n = list; n; n = n->next) printf("%d ", n->value);
    putchar('\n');
    list_free(list);

    /* --- fibonacci --- */
    puts("[2] fibonacci");
    for (int i = 0; i <= 10; i++)
        printf("  fib(%2d) = %ld\n", i, fib(i));

    /* --- sorting --- */
    puts("[3] bubble sort");
    int arr[] = { 64, 25, 12, 22, 11 };
    int len   = (int)(sizeof arr / sizeof arr[0]);
    bubble_sort(arr, len);
    printf("  sorted: ");
    for (int i = 0; i < len; i++) printf("%d ", arr[i]);
    putchar('\n');

    /* --- strings --- */
    puts("[4] strings");
    string_demo();

    /* --- math --- */
    puts("[5] math");
    math_demo();

    /* --- wall clock --- */
    puts("[6] time");
    time_t now = time(NULL);
    printf("  time_t = %ld\n", (long)now);

    puts("=== PASS ===");
    return 0;
}
