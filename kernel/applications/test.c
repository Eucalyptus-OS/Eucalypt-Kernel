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

    print("Waiting 5 seconds...\n");
    sleep(5000);
    print("Done waiting!\n");
    
    return 0;
}