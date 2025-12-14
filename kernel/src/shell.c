#include <shell.h>
#include <ramdisk/ramfs.h>
#include <x86_64/allocator/heap.h>
#include <string.h>
#include <stdint.h>
#include <flanterm/flanterm.h>

extern struct flanterm_context *ft_ctx;

#define MAX_INPUT_LENGTH 128
#define MAX_PARAMS 4
#define MAX_PARAM_LENGTH 64

char input_buffer[MAX_INPUT_LENGTH + 1];
char params[MAX_PARAMS][MAX_PARAM_LENGTH];
int param_count = 0;
file_system_t global_fs;

typedef void (*command_handler_func)(void);

void help_command(void);
void clear_command(void);
void listfs_command(void);
void readfile_command(void);
void writefile_command(void);
void handle_backspace(void);

typedef struct {
    const char *name;
    command_handler_func handler;
} command_t;

const command_t command_table[] = {
    {"help", help_command},
    {"clear", clear_command},
    {"listfs", listfs_command},
    {"readfile", readfile_command},
    {"writefile", writefile_command},
};

#define NUM_COMMANDS (sizeof(command_table) / sizeof(command_t))

size_t input_pos = 0;

void shell_init(void) {
    init_ramfs(&global_fs);
    flanterm_write(ft_ctx, "Shell initialized\n> ");
}

void help_command() {
    flanterm_write(ft_ctx, "Available commands:\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        flanterm_write(ft_ctx, "  ");
        flanterm_write(ft_ctx, command_table[i].name);
        flanterm_write(ft_ctx, "\n");
    }
}

void parse_params(char *command) {
    param_count = 0;
    memset(params, 0, sizeof(params));
    
    int in_space = 1;
    int param_pos = 0;
    
    for (int i = 0; command[i] != '\0'; i++) {
        if (command[i] == ' ') {
            in_space = 1;
            param_pos = 0;
        } else {
            if (in_space) {
                if (param_count < MAX_PARAMS) {
                    param_count++;
                }
                in_space = 0;
            }
            if (param_count > 0 && param_count <= MAX_PARAMS && param_pos < MAX_PARAM_LENGTH - 1) {
                params[param_count - 1][param_pos++] = command[i];
            }
        }
    }
}

void compare_command(char *command) {
    if (command == NULL || command[0] == '\0') {
        return;
    }
    
    parse_params(command);
    
    if (param_count == 0) {
        return;
    }
    
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(params[0], command_table[i].name) == 0) {
            command_table[i].handler();
            return;
        }
    }
    
    flanterm_write(ft_ctx, "Unknown command: ");
    flanterm_write(ft_ctx, params[0]);
    flanterm_write(ft_ctx, "\n");
}

void shell_print(uint32_t v) {
    if (v > 0xFF) {
        return;
    }
    
    char c = (char)v;
    
    if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            input_pos--;
            input_buffer[input_pos] = '\0';
            flanterm_write(ft_ctx, "\b \b");
        }
    }
    else if (c == '\n' || c == '\r') {
        input_buffer[input_pos] = '\0';
        flanterm_write(ft_ctx, "\n");
        compare_command(input_buffer);
        memset(input_buffer, 0, sizeof(input_buffer));
        input_pos = 0;
        flanterm_write(ft_ctx, "> ");
    }
    else if (c >= 32 && c <= 126 && input_pos < MAX_INPUT_LENGTH) {
        input_buffer[input_pos] = c;
        input_pos++;
        char buf[2];
        buf[0] = c;
        buf[1] = '\0';
        flanterm_write(ft_ctx, buf);
    }
}

void handle_backspace() {
    if (input_pos > 0) {
        input_pos--;
        input_buffer[input_pos] = '\0';
        flanterm_write(ft_ctx, "\b \b");
    }
}

void clear_command(void) {
    flanterm_write(ft_ctx, "\033[2J\033[H");
    input_pos = 0;
    memset(input_buffer, 0, sizeof(input_buffer));
}

void listfs_command(void) {
    char *buf = (char *)kmalloc(1024);
    
    if (buf == NULL) {
        flanterm_write(ft_ctx, "Memory allocation failed\n");
        return;
    }
    
    list_files(&global_fs, buf);
    
    flanterm_write(ft_ctx, "Files:\n");
    for (uint32_t i = 0; i < global_fs.file_count; i++) {
        flanterm_write(ft_ctx, "  ");
        flanterm_write(ft_ctx, buf + (i * MAX_NAME_LENGTH));
        flanterm_write(ft_ctx, "\n");
    }
    
    kfree(buf);
}

void readfile_command(void) {
    if (param_count < 2) {
        flanterm_write(ft_ctx, "Usage: readfile <filename>\n");
        return;
    }
    
    char *buf = (char *)kmalloc(4096);
    if (!buf) {
        flanterm_write(ft_ctx, "Memory allocation failed\n");
        return;
    }
    
    memset(buf, 0, 4096);
    read_file(params[1], buf, 4096, &global_fs);
    flanterm_write(ft_ctx, "Content: \n");
    flanterm_write(ft_ctx, buf);
    flanterm_write(ft_ctx, "\n");
    
    kfree(buf);
}

void writefile_command(void) {
    if (param_count < 3) {
        flanterm_write(ft_ctx, "Usage: writefile <filename> <content>\n");
        return;
    }
    
    char *content_buf = (char *)kmalloc(256);
    if (!content_buf) {
        flanterm_write(ft_ctx, "Memory allocation failed\n");
        return;
    }
    
    memset(content_buf, 0, 256);
    size_t pos = 0;
    
    for (int i = 2; i < param_count && pos < 255; i++) {
        if (i > 2) {
            content_buf[pos++] = ' ';
        }
        size_t param_len = strlen(params[i]);
        for (size_t j = 0; j < param_len && pos < 255; j++) {
            content_buf[pos++] = params[i][j];
        }
    }
    
    write_file(params[1], (uint8_t *)content_buf, strlen(content_buf) + 1, &global_fs);
    flanterm_write(ft_ctx, "File written successfully\n");
    
    kfree(content_buf);
}