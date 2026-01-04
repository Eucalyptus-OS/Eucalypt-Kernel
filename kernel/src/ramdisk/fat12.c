#include <ramdisk/fat12.h>
#include <ramdisk/ramdisk.h>
#include <x86_64/allocator/heap.h>
#include <stdint.h>
#include <string.h>

typedef struct bpb {
	uint8_t jmp_boot[3];
	uint8_t oem_name[8];
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sector_count;
	uint8_t num_fats;
	uint16_t root_entry_count;
	uint16_t total_sectors_16;
	uint8_t media;
	uint16_t fat_size_16;
	uint16_t sectors_per_track;
	uint16_t num_heads;
	uint32_t hidden_sectors;
	uint32_t total_sectors_32;
	uint8_t drive_number;
	uint8_t reserved1;
	uint8_t boot_sig;
	uint32_t volume_id;
	uint8_t volume_label[11];
	uint8_t file_system_type[8];
	uint8_t boot_code[448];
	uint16_t boot_sector_signature;
} __attribute__((packed)) bpb_t;

typedef struct fat12 {
	bpb_t *bpb;
	uint8_t *fat;
	uint32_t fat_start_sector;
	uint32_t root_dir_start_sector;
	uint32_t data_start_sector;
	uint32_t root_dir_sectors;
} __attribute__((packed)) fat12_t;

static fat12_t *g_fat12 = NULL;

uint16_t read_fat_entry(uint16_t cluster) {
	uint32_t fat_offset = cluster + (cluster / 2);
	uint32_t fat_sector = g_fat12->fat_start_sector + (fat_offset / g_fat12->bpb->bytes_per_sector);
	uint32_t entry_offset = fat_offset % g_fat12->bpb->bytes_per_sector;

	uint8_t *buffer = kmalloc(g_fat12->bpb->bytes_per_sector);
	read_ramdisk_sector(fat_sector, buffer);

	uint16_t value;
	if (cluster & 1) {
		value = (*(uint16_t *)&buffer[entry_offset]) >> 4;
	} else {
		value = (*(uint16_t *)&buffer[entry_offset]) & 0x0FFF;
	}

	kfree(buffer);
	return value;
}

void write_fat_entry(uint16_t cluster, uint16_t value) {
	uint32_t fat_offset = cluster + (cluster / 2);
	uint32_t fat_sector = g_fat12->fat_start_sector + (fat_offset / g_fat12->bpb->bytes_per_sector);
	uint32_t entry_offset = fat_offset % g_fat12->bpb->bytes_per_sector;

	uint8_t *buffer = kmalloc(g_fat12->bpb->bytes_per_sector);
	read_ramdisk_sector(fat_sector, buffer);

	if (cluster & 1) {
		uint16_t current = *(uint16_t *)&buffer[entry_offset];
		current = (current & 0x000F) | (value << 4);
		*(uint16_t *)&buffer[entry_offset] = current;
	} else {
		uint16_t current = *(uint16_t *)&buffer[entry_offset];
		current = (current & 0xF000) | (value & 0x0FFF);
		*(uint16_t *)&buffer[entry_offset] = current;
	}

	write_ramdisk_sector(fat_sector, buffer);
	kfree(buffer);

	for (int i = 1; i < g_fat12->bpb->num_fats; i++) {
		uint32_t mirror_sector = fat_sector + (g_fat12->bpb->fat_size_16 * i);
		write_ramdisk_sector(mirror_sector, buffer);
	}
}

uint16_t allocate_cluster() {
	for (uint16_t cluster = 2; cluster < ((g_fat12->bpb->fat_size_16 * g_fat12->bpb->bytes_per_sector * 2) / 3); cluster++) {
		uint16_t value = read_fat_entry(cluster);
		if (value == 0) {
			write_fat_entry(cluster, 0xFFF);
			return cluster;
		}
	}
	return 0;
}

void free_cluster_chain(uint16_t start_cluster) {
	uint16_t cluster = start_cluster;
	while (cluster >= 2 && cluster < 0xFF8) {
		uint16_t next = read_fat_entry(cluster);
		write_fat_entry(cluster, 0);
		cluster = next;
	}
}

uint32_t cluster_to_sector(uint16_t cluster) {
	return g_fat12->data_start_sector + ((cluster - 2) * g_fat12->bpb->sectors_per_cluster);
}

dir_entry_t *find_file(const char *filename) {
	uint8_t *buffer = kmalloc(g_fat12->bpb->bytes_per_sector);
	uint32_t entries_per_sector = g_fat12->bpb->bytes_per_sector / sizeof(dir_entry_t);
	
	for (uint32_t i = 0; i < g_fat12->root_dir_sectors; i++) {
		read_ramdisk_sector(g_fat12->root_dir_start_sector + i, buffer);
		
		for (uint32_t j = 0; j < entries_per_sector; j++) {
			dir_entry_t *entry = (dir_entry_t *)(buffer + j * sizeof(dir_entry_t));
			
			if (entry->name[0] == 0x00) {
				kfree(buffer);
				return NULL;
			}
			
			if (entry->name[0] == 0xE5) {
				continue;
			}
			
			char name[12];
			int k;
			for (k = 0; k < 8 && entry->name[k] != ' '; k++) {
				name[k] = entry->name[k];
			}
			if (entry->ext[0] != ' ') {
				name[k++] = '.';
				for (int l = 0; l < 3 && entry->ext[l] != ' '; l++) {
					name[k++] = entry->ext[l];
				}
			}
			name[k] = '\0';
			
			if (strcmp(name, filename) == 0) {
				dir_entry_t *result = kmalloc(sizeof(dir_entry_t));
				memcpy(result, entry, sizeof(dir_entry_t));
				kfree(buffer);
				return result;
			}
		}
	}
	
	kfree(buffer);
	return NULL;
}

dir_entry_t *create_file(const char *filename) {
	uint8_t *buffer = kmalloc(g_fat12->bpb->bytes_per_sector);
	uint32_t entries_per_sector = g_fat12->bpb->bytes_per_sector / sizeof(dir_entry_t);
	filename = toupper(filename);
	for (uint32_t i = 0; i < g_fat12->root_dir_sectors; i++) {
		read_ramdisk_sector(g_fat12->root_dir_start_sector + i, buffer);
		
		for (uint32_t j = 0; j < entries_per_sector; j++) {
			dir_entry_t *entry = (dir_entry_t *)(buffer + j * sizeof(dir_entry_t));
			
			if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
				memset(entry, 0, sizeof(dir_entry_t));
				
				int k = 0;
				while (filename[k] && filename[k] != '.' && k < 8) {
					entry->name[k] = filename[k];
					k++;
				}
				while (k < 8) {
					entry->name[k++] = ' ';
				}
				
				const char *ext_ptr = filename;
				while (*ext_ptr && *ext_ptr != '.') ext_ptr++;
				if (*ext_ptr == '.') ext_ptr++;
				
				k = 0;
				while (ext_ptr[k] && k < 3) {
					entry->ext[k] = ext_ptr[k];
					k++;
				}
				while (k < 3) {
					entry->ext[k++] = ' ';
				}
				
				entry->attr = 0x20;
				entry->first_cluster_low = 0;
				entry->file_size = 0;
				
				write_ramdisk_sector(g_fat12->root_dir_start_sector + i, buffer);
				
				dir_entry_t *result = kmalloc(sizeof(dir_entry_t));
				memcpy(result, entry, sizeof(dir_entry_t));
				kfree(buffer);
				return result;
			}
		}
	}
	
	kfree(buffer);
	return NULL;
}

uint8_t *read_file(dir_entry_t *file, uint32_t *size) {
	if (!file || file->file_size == 0) {
		*size = 0;
		return NULL;
	}
	
	*size = file->file_size;
	uint8_t *data = kmalloc(file->file_size);
	uint32_t bytes_read = 0;
	uint16_t cluster = file->first_cluster_low;
	
	while (cluster >= 2 && cluster < 0xFF8 && bytes_read < file->file_size) {
		uint32_t sector = cluster_to_sector(cluster);
		uint8_t *buffer = kmalloc(g_fat12->bpb->bytes_per_sector * g_fat12->bpb->sectors_per_cluster);
		
		for (int i = 0; i < g_fat12->bpb->sectors_per_cluster; i++) {
			read_ramdisk_sector(sector + i, buffer + i * g_fat12->bpb->bytes_per_sector);
		}
		
		uint32_t to_copy = (file->file_size - bytes_read) > (g_fat12->bpb->bytes_per_sector * g_fat12->bpb->sectors_per_cluster) ? 
                               (g_fat12->bpb->bytes_per_sector * g_fat12->bpb->sectors_per_cluster) : (file->file_size - bytes_read);
		memcpy(data + bytes_read, buffer, to_copy);
		bytes_read += to_copy;
		
		kfree(buffer);
		cluster = read_fat_entry(cluster);
	}
	
	return data;
}

void write_file(const char *filename, uint8_t *data) {
	dir_entry_t *file = find_file(filename);
	
	uint32_t size = 0;
	while (data[size] != '\0') {
		size++;
	}
	
	if (!file) {
		file = create_file(filename);
	} else {
		if (file->first_cluster_low) {
			free_cluster_chain(file->first_cluster_low);
		}
	}
	
	if (!file) return;
	
	file->file_size = size;
	
	uint32_t bytes_written = 0;
	uint16_t prev_cluster = 0;
	uint16_t first_cluster = 0;
	
	while (bytes_written < size) {
		uint16_t cluster = allocate_cluster();
		if (cluster == 0) break;
		
		if (first_cluster == 0) {
			first_cluster = cluster;
		}
		
		if (prev_cluster != 0) {
			write_fat_entry(prev_cluster, cluster);
		}
		
		uint32_t sector = cluster_to_sector(cluster);
		uint32_t to_write = (size - bytes_written) > (g_fat12->bpb->bytes_per_sector * g_fat12->bpb->sectors_per_cluster) ?
		                    (g_fat12->bpb->bytes_per_sector * g_fat12->bpb->sectors_per_cluster) : (size - bytes_written);
		
		uint8_t *buffer = kmalloc(g_fat12->bpb->bytes_per_sector * g_fat12->bpb->sectors_per_cluster);
		memcpy(buffer, data + bytes_written, to_write);
		
		for (int i = 0; i < g_fat12->bpb->sectors_per_cluster; i++) {
			write_ramdisk_sector(sector + i, buffer + i * g_fat12->bpb->bytes_per_sector);
		}
		
		kfree(buffer);
		bytes_written += to_write;
		prev_cluster = cluster;
	}
	
	file->first_cluster_low = first_cluster;
	
	uint8_t *root_buffer = kmalloc(g_fat12->bpb->bytes_per_sector);
	uint32_t entries_per_sector = g_fat12->bpb->bytes_per_sector / sizeof(dir_entry_t);
	
	for (uint32_t i = 0; i < g_fat12->root_dir_sectors; i++) {
		read_ramdisk_sector(g_fat12->root_dir_start_sector + i, root_buffer);
		
		for (uint32_t j = 0; j < entries_per_sector; j++) {
			dir_entry_t *entry = (dir_entry_t *)(root_buffer + j * sizeof(dir_entry_t));
			
			int match = 1;
			for (int k = 0; k < 8; k++) {
				if (entry->name[k] != file->name[k]) {
					match = 0;
					break;
				}
			}
			if (match) {
				for (int k = 0; k < 3; k++) {
					if (entry->ext[k] != file->ext[k]) {
						match = 0;
						break;
					}
				}
			}
			
			if (match) {
				memcpy(entry, file, sizeof(dir_entry_t));
				write_ramdisk_sector(g_fat12->root_dir_start_sector + i, root_buffer);
				kfree(root_buffer);
				kfree(file);
				return;
			}
		}
	}
	kfree(root_buffer);
	kfree(file);
}

void init_fat12() {
    g_fat12 = kmalloc(sizeof(fat12_t));
    g_fat12->bpb = kmalloc(sizeof(bpb_t));
    
    read_ramdisk_sector(0, (uint8_t *)g_fat12->bpb);
    
    g_fat12->fat_start_sector = g_fat12->bpb->reserved_sector_count;
    g_fat12->root_dir_sectors = ((g_fat12->bpb->root_entry_count * 32) + (g_fat12->bpb->bytes_per_sector - 1)) / g_fat12->bpb->bytes_per_sector;
    g_fat12->root_dir_start_sector = g_fat12->fat_start_sector + (g_fat12->bpb->num_fats * g_fat12->bpb->fat_size_16);
    g_fat12->data_start_sector = g_fat12->root_dir_start_sector + g_fat12->root_dir_sectors;
}