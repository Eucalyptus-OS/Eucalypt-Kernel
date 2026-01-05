#include "sys.h"
#include <stdint.h>
#include <stddef.h>

int main() {
    print("Before malloc\n");
    void *ptr = malloc(1024);
    
    if (ptr == 0) {
        print("malloc returned NULL!\n");
    } else {
        print("malloc returned valid pointer\n");
        free(ptr);
    }

    while (1) {
        malloc(1024);
    }
    
    return 0;
}