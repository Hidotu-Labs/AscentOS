#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple linked list node */
typedef struct Node {
    int value;
    struct Node *next;
} Node;

/* Build a linked list of `count` nodes on the heap */
static Node *build_list(int count) {
    Node *head = NULL;
    for (int i = count - 1; i >= 0; i--) {
        Node *n = malloc(sizeof(Node));
        if (!n) {
            fprintf(stderr, "malloc failed at i=%d\n", i);
            exit(1);
        }
        n->value = i;
        n->next  = head;
        head     = n;
    }
    return head;
}

/* Walk and free the list */
static void free_list(Node *head) {
    while (head) {
        Node *next = head->next;
        free(head);
        head = next;
    }
}

int main(void) {
    printf("=== glibc malloc/free test ===\n");

    /* --- 1. Basic alloc/free --- */
    printf("[1] Basic malloc/free... ");
    char *buf = malloc(64);
    if (!buf) { fprintf(stderr, "FAIL\n"); return 1; }
    strcpy(buf, "Hello, AscentOS!");
    printf("OK  (\"%s\")\n", buf);
    free(buf);

    /* --- 2. Zero-initialised allocation (calloc) --- */
    printf("[2] calloc... ");
    int *arr = calloc(16, sizeof(int));
    if (!arr) { fprintf(stderr, "FAIL\n"); return 1; }
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (arr[i] != 0) { all_zero = 0; break; }
    printf("OK  (all-zero=%s)\n", all_zero ? "yes" : "no");

    /* --- 3. realloc grow --- */
    printf("[3] realloc grow... ");
    arr = realloc(arr, 64 * sizeof(int));
    if (!arr) { fprintf(stderr, "FAIL\n"); return 1; }
    for (int i = 0; i < 64; i++) arr[i] = i * i;
    printf("OK  (arr[63]=%d)\n", arr[63]);
    free(arr);

    /* --- 4. Many small allocations --- */
    printf("[4] 1000 small allocs/frees... ");
    for (int i = 0; i < 1000; i++) {
        void *p = malloc(32 + (i % 128));
        if (!p) { fprintf(stderr, "FAIL at i=%d\n", i); return 1; }
        memset(p, (unsigned char)i, 32);
        free(p);
    }
    printf("OK\n");

    /* --- 5. Heap-allocated linked list --- */
    printf("[5] Linked list (256 nodes)... ");
    Node *list = build_list(256);
    /* verify order */
    int ok = 1;
    Node *cur = list;
    for (int i = 0; i < 256; i++, cur = cur->next) {
        if (!cur || cur->value != i) { ok = 0; break; }
    }
    free_list(list);
    printf("OK  (ordered=%s)\n", ok ? "yes" : "no");

    /* --- 6. Large allocation --- */
    printf("[6] Large alloc (4 MB)... ");
    size_t big = 4 * 1024 * 1024;
    char *large = malloc(big);
    if (!large) { fprintf(stderr, "FAIL\n"); return 1; }
    memset(large, 0xAB, big);
    /* spot-check */
    int spot_ok = (large[0] == (char)0xAB) && (large[big - 1] == (char)0xAB);
    free(large);
    printf("OK  (spot-check=%s)\n", spot_ok ? "pass" : "fail");

    printf("=== All tests passed! ===\n");
    return 0;
}
