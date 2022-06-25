#include "nanoemu.h"

size_t
read_file(FILE* f, uint8_t** r) {
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* content = malloc(fsize + 1);
    fread(content, fsize, 1, f);
    fclose(f);
    content[fsize] = 0;
    *r = content;

    return fsize;
}
