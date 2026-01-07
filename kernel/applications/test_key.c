#include "sys.h"
#include "stdio.h"

int main() {
    printf("Key test - press keys (ESC to quit)\n");
    
    for (int i = 0; i < 1000; i++) {
        uint32_t key = get_key();
        
        if (key != 0) {
            printf("Got key: %u (0x%x)\n", key, key);
            
            if (key == 27) break;
        }
        
        sleep(50);
    }
    
    printf("Test done\n");
    return 0;
}