#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// Test emmalloc internals, but through the external interface. We expect
// very specific outputs here based on the internals, this test would not
// pass in another malloc.

void check_where_we_would_malloc(size_t size, void* expected) {
  void* temp = malloc(size);
  assert(temp == expected);
  free(temp);
}

int main() {
  printf("allocate 0\n");
  void* ptr = malloc(0);
  assert(ptr == 0);
  printf("allocate 100\n");
  void* first = malloc(100);
  printf("free 100\n");
  free(first);
  printf("allocate another 100\n");
  void* second = malloc(100);
  printf("allocate 10\n");
  assert(second == first);
  void* third = malloc(10);
  printf("%d - %d\n", size_t(third), size_t(first));
  assert(size_t(third) == size_t(first) + 112 + 16); // allocation units are multiples of 16, so first allocates a payload of 112. then second has 16 of metadata
  printf("allocate 10 more\n");
  void* four = malloc(10);
  assert(size_t(four) == size_t(third) + 16 + 16); // allocation units are multiples of 16, so first allocates a payload of 16. then second has 16 of metadata
  printf("free the first\n");
  free(second);
  // we reuse the first area, despite stuff later.
  for (int i = 0; i < 10; i++) {
    check_where_we_would_malloc(100, first);
  }
  printf("free everything\n");
  free(third);
  free(four);
  printf("allocate various sizes to see they all start at the start\n");
  for (int i = 1; i < 300; i++) {
    printf("%d\n", i);
    check_where_we_would_malloc(i, first);
  }
  puts("ok");
}

