#ifndef ASTRA_A_COMPILER_H
#define ASTRA_A_COMPILER_H
#include "../tcc.h"
/* A -> machine code via Astra (no C intermediate). Modular/extensible. */
ST_FUNC int tcc_compile_a(TCCState *s1, const char *filename, const char *buf, int len);
#endif
