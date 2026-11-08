/* i8086 shares the i386 linker; only the ELF load address differs (boot
 * image is loaded at 0x7C00 instead of 0x08048000). This file exists so
 * libastra.c/astra.h can include the target linker by name. */
#define ELF_START_ADDR 0x00007C00
#include "i386-link.c"
