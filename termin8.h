#ifndef __TERMIN8_H__
#define __TERMIN8_H__

#include <stdio.h>
#include <string.h>
#include <errno.h>

#define VEXOF(V, F)  V = F; if(-1 == V) exof(__FILE__, __LINE__)
#define EXOF(F)      if(-1 == F) exof(__FILE__, __LINE__)

static void exof(char *file, int line) {
  fprintf(stderr, "%s[%d]: %s\n", file, line, strerror(errno));
  exit(1);
}

#endif
