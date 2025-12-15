#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

typedef struct dir_entry {
	uint8_t name[8];
	uint8_t ext[3];
	uint8_t attr;
	uint8_t reserved;
	uint8_t create_time_tenth;
	uint16_t create_time;
	uint16_t create_date;
	uint16_t last_access_date;
	uint16_t first_cluster_high;
	uint16_t write_time;
	uint16_t write_date;
	uint16_t first_cluster_low;
	uint32_t file_size;
} __attribute__((packed)) dir_entry_t;

void init_fat12();
uint16_t read_fat_entry(uint16_t cluster);
void write_fat_entry(uint16_t cluster, uint16_t value);
uint16_t allocate_cluster();
void free_cluster_chain(uint16_t start_cluster);
uint32_t cluster_to_sector(uint16_t cluster);
dir_entry_t *find_file(const char *filename);
dir_entry_t *create_file(const char *filename);
uint8_t *read_file(dir_entry_t *file, uint32_t *size);
void write_file(const char *filename, uint8_t *data, uint32_t size);

#endif