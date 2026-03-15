/// Unfinished elf parser

use framebuffer::println;
use vfs::{self, vfs_file_exists, vfs_read_file};

#[allow(unused)]
fn parse_elf(filename: &str) {
    if !vfs_file_exists(filename) {
        println!("File does not exist: {}", filename);
        return;
    }

    match vfs_read_file(filename) {
        Ok(contents) => {
            if contents.len() >= 4 && &contents[0..4] == b"\x7fELF" {
                println!("{} is a valid ELF file!", filename);
            } else {
                println!("{} is not an ELF file.", filename);
            }
        }
        Err(e) => {
            println!("Failed to read file {}: {}", filename, e);
        }
    }

    
}
