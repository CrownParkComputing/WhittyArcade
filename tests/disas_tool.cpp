#include "m68k.h"
#include <cstdio>
#include <cstring>
int main(int argc, char**argv) {
    if (argc < 3) { std::fprintf(stderr,"usage: %s <bin> 0xaddr [n]\n",argv[0]); return 1; }
    FILE*f = fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    auto *buf = new unsigned char[sz]; fread(buf,1,sz,f); fclose(f);
    unsigned base = 0; std::sscanf(argv[2],"0x%x",&base);
    int n = argc > 3 ? atoi(argv[3]) : 32;
    m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    char out[128]; unsigned pc = base;
    for (int i = 0; i < n; ++i) {
        unsigned off = (pc - 0) & 0x3fffff;
        if (off+4 > (unsigned)sz) break;
        unsigned got = m68k_disassemble_raw(out, pc, buf+off, buf+off, M68K_CPU_TYPE_68000);
        std::printf("0x%06x: %s\n", pc, out);
        pc += got;
    }
    delete[] buf; return 0;
}
