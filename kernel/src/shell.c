#include <shell.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/execute.h>
#include <ramdisk/ramdisk.h>
#include <ramdisk/fat12.h>
#include <string.h>
#include <stdint.h>
#include <flanterm/flanterm.h>

extern struct flanterm_context *ft_ctx;

#define MAX_INPUT_LENGTH 128
#define MAX_PARAMS 4
#define MAX_PARAM_LENGTH 64

// Magic number to identify executables: "BIN" in little-endian + null
#define EXEC_MAGIC 0x004E4942  // 0x42='B', 0x49='I', 0x4E='N', 0x00=null

char input_buffer[MAX_INPUT_LENGTH + 1];
char params[MAX_PARAMS][MAX_PARAM_LENGTH];
int param_count = 0;

typedef void (*command_handler_func)(void);

void help_command(void);
void clear_command(void);
void ls_command(void);
void rdf_command(void);
void wef_command(void);
void exec_command(void);
void handle_backspace(void);

typedef struct {
	const char *name;
	const char *desc;
	command_handler_func handler;
} command_t;

const command_t command_table[] = {
	{"help", "Displays this message", help_command},
	{"clear", "Clears the screen", clear_command},
	{"ls","List all the files in the fs", ls_command},
	{"rdf", "Read a file", rdf_command},
	{"wef", "Write a file", wef_command},
	{"exec", "Execute a file", exec_command},
};

#define NUM_COMMANDS (sizeof(command_table) / sizeof(command_t))

size_t input_pos = 0;

void help_command() {
	flanterm_write(ft_ctx, "Available commands:\n");
	for (size_t i = 0; i < NUM_COMMANDS; i++) {
		flanterm_write(ft_ctx, "  ");
		flanterm_write(ft_ctx, command_table[i].name);
		flanterm_write(ft_ctx, ": ");
		flanterm_write(ft_ctx, command_table[i].desc);
		flanterm_write(ft_ctx, "\n");
	}
	flanterm_write(ft_ctx, "You can also execute files directly by typing their name\n");
	flanterm_write(ft_ctx, "Example: > MYAPP\n");
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

void try_execute_as_file(const char *filename) {
	dir_entry_t *file = find_file(filename);
	if (!file) {
		flanterm_write(ft_ctx, "Unknown command: ");
		flanterm_write(ft_ctx, filename);
		flanterm_write(ft_ctx, "\n");
		return;
	}
	
	uint32_t size;
	uint8_t *data = read_file(file, &size);
	
	if (!data || size == 0) {
		flanterm_write(ft_ctx, "File found but could not be read: ");
		flanterm_write(ft_ctx, filename);
		flanterm_write(ft_ctx, "\n");
		kfree(file);
		return;
	}
	
	flanterm_write(ft_ctx, "Executing ");
	flanterm_write(ft_ctx, filename);
	flanterm_write(ft_ctx, "...\n");
	
	// load_and_execute_app now handles the header check internally
	int result = load_and_execute_app(data, size);
	
	kfree(data);
	kfree(file);
	
	if (result == 0) {
		flanterm_write(ft_ctx, "Application returned\n");
	} else {
		flanterm_write(ft_ctx, "Error: Failed to execute ");
		flanterm_write(ft_ctx, filename);
		flanterm_write(ft_ctx, " (code ");
		char code[8];
		int_to_str(-result, code);
		flanterm_write(ft_ctx, code);
		flanterm_write(ft_ctx, ")\n");
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
	
	try_execute_as_file(params[0]);
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

void ls_command(void) {
	uint8_t *buffer = kmalloc(512);
	int found_files = 0;
	
	flanterm_write(ft_ctx, "Files in root directory:\n");
	
	for (uint32_t sector = 0; sector < 14; sector++) {
		read_ramdisk_sector(19 + sector, buffer);
		
		for (int i = 0; i < 16; i++) {
			dir_entry_t *entry = (dir_entry_t *)(buffer + i * 32);
			
			if (entry->name[0] == 0x00) {
				kfree(buffer);
				if (found_files == 0) {
					flanterm_write(ft_ctx, "  (empty)\n");
				}
				return;
			}
			
			if (entry->name[0] == 0xE5) {
				continue;
			}
			
			if (entry->attr & 0x08) {
				continue;
			}
			
			found_files = 1;
			flanterm_write(ft_ctx, "  ");
			
			for (int j = 0; j < 8 && entry->name[j] != ' '; j++) {
				char c[2] = {entry->name[j], '\0'};
				flanterm_write(ft_ctx, c);
			}
			
			if (entry->ext[0] != ' ') {
				flanterm_write(ft_ctx, ".");
				for (int j = 0; j < 3 && entry->ext[j] != ' '; j++) {
					char c[2] = {entry->ext[j], '\0'};
					flanterm_write(ft_ctx, c);
				}
			}
			
			flanterm_write(ft_ctx, " (");
			char size_buf[16];
			int_to_str(entry->file_size, size_buf);
			flanterm_write(ft_ctx, size_buf);
			flanterm_write(ft_ctx, " bytes)\n");
		}
	}
	
	kfree(buffer);
	if (found_files == 0) {
		flanterm_write(ft_ctx, "  (empty)\n");
	}
}

void rdf_command(void) {
	if (param_count < 2) {
		flanterm_write(ft_ctx, "Usage: rdf <filename>\n");
		return;
	}
	
	dir_entry_t *file = find_file(params[1]);
	if (!file) {
		flanterm_write(ft_ctx, "File not found: ");
		flanterm_write(ft_ctx, params[1]);
		flanterm_write(ft_ctx, "\n");
		return;
	}
	
	uint32_t size;
	uint8_t *data = read_file(file, &size);
	
	if (!data || size == 0) {
		flanterm_write(ft_ctx, "File is empty or could not be read\n");
		kfree(file);
		return;
	}
	
	flanterm_write(ft_ctx, "Contents of ");
	flanterm_write(ft_ctx, params[1]);
	flanterm_write(ft_ctx, ":\n");
	
	for (uint32_t i = 0; i < size; i++) {
		char c[2] = {data[i], '\0'};
		flanterm_write(ft_ctx, c);
	}
	
	flanterm_write(ft_ctx, "\n");
	
	kfree(data);
	kfree(file);
}

void wef_command(void) {
	if (param_count < 3) {
		flanterm_write(ft_ctx, "Usage: wef <filename> <content>\n");
		return;
	}
	
	char content[256];
	memset(content, 0, sizeof(content));
	int content_pos = 0;
	
	for (int i = 2; i < param_count && content_pos < 255; i++) {
		if (i > 2) {
			content[content_pos++] = ' ';
		}
		for (int j = 0; params[i][j] != '\0' && content_pos < 255; j++) {
			content[content_pos++] = params[i][j];
		}
	}
	
	write_file(params[1], (uint8_t *)content, content_pos);
	
	flanterm_write(ft_ctx, "Wrote ");
	char size_buf[16];
	int_to_str(content_pos, size_buf);
	flanterm_write(ft_ctx, size_buf);
	flanterm_write(ft_ctx, " bytes to ");
	flanterm_write(ft_ctx, params[1]);
	flanterm_write(ft_ctx, "\n");
}

void exec_command(void) {
    if (param_count < 2) {
        flanterm_write(ft_ctx, "Usage: exec <filename>\n");
        return;
    }
    
    dir_entry_t *file = find_file(params[1]);
    if (!file) {
        flanterm_write(ft_ctx, "File not found: ");
        flanterm_write(ft_ctx, params[1]);
        flanterm_write(ft_ctx, "\n");
        return;
    }
    
    uint32_t size;
    uint8_t *data = read_file(file, &size);
    
    if (!data || size == 0) {
        flanterm_write(ft_ctx, "File is empty or could not be read\n");
        kfree(file);
        return;
    }
    
    flanterm_write(ft_ctx, "Executing ");
    flanterm_write(ft_ctx, params[1]);
    flanterm_write(ft_ctx, "...\n");
    
    // load_and_execute_app now handles the header check internally
    int result = load_and_execute_app(data, size);
    
    kfree(data);
    kfree(file);
    
    if (result == 0) {
        flanterm_write(ft_ctx, "Application returned\n");
    } else {
        flanterm_write(ft_ctx, "Error: Failed to execute ");
        flanterm_write(ft_ctx, params[1]);
        flanterm_write(ft_ctx, " (code ");
        char code[8];
        int_to_str(-result, code);
        flanterm_write(ft_ctx, code);
        flanterm_write(ft_ctx, ")\n");
    }
}