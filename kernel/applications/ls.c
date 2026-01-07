#include "sys.h"

void main() {
    file_info_t *buf = (file_info_t *)malloc(sizeof(file_info_t) * 255);
    
    if (!buf) {
        print("Memory allocation failed\n");
        return;
    }
    
    int64_t count = ls(buf, 255);
    
    if (count < 0) {
        print("Error listing files\n");
        free(buf);
        return;
    }
    
    print("Files in root directory:\n");
    
    if (count == 0) {
        print("  (empty)\n");
    } else {
        for (int64_t i = 0; i < count; i++) {
            print("  ");
            print(buf[i].name);
            print(" (");
            
            char size_str[16];
            uint32_t size = buf[i].size;
            int pos = 0;
            
            if (size == 0) {
                size_str[pos++] = '0';
            } else {
                char temp[16];
                int temp_pos = 0;
                while (size > 0) {
                    temp[temp_pos++] = '0' + (size % 10);
                    size /= 10;
                }
                for (int j = temp_pos - 1; j >= 0; j--) {
                    size_str[pos++] = temp[j];
                }
            }
            size_str[pos] = '\0';
            
            print(size_str);
            print(" bytes)\n");
        }
    }
    
    free(buf);
}