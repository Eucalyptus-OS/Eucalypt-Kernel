#pragma once

#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04
#define PROT_NONE   0x00

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x04
#define MAP_ANONYMOUS  0x08
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_GROWSDOWN  0x10
#define MAP_STACK      0x20
#define MAP_NORESERVE  0x40
