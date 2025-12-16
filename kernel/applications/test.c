#include "sys.h"
#include <stdint.h>
#include <stddef.h>

void _start(void) {
    print("Allocating...\n");

    char *buf = malloc(1024);
    
    if (buf) {
        buf[0]='D'; buf[1]='o'; buf[2]='n'; buf[3]='e'; buf[4]='\n'; buf[5]='\0';

        print(buf);
        free(buf);
    }
}
