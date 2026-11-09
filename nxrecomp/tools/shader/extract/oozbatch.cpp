// oozbatch: ooz's Oodle decoder (deps/ooz, github.com/powzix/ooz, GPLv3 or later) behind a stdin/stdout batch
// protocol for extract.py: [u64 decompressed size][u32 length][data] in, [u32 length (0: failed)][data] out,
// until EOF. Because it includes ooz, this program is GPLv3 or later; it runs as its own process, nothing links it.
#define main ooz_main
#include "kraken.cpp"
#undef main
#include <fcntl.h>
#include <io.h>

static bool rd(void *p, size_t n) { return fread(p, 1, n, stdin) == n; }

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    for (;;) {
        unsigned long long out;
        unsigned int in;
        if (!rd(&out, 8) || !rd(&in, 4)) return 0;
        byte *src = new byte[in + 64];
        if (!rd(src, in)) return 1;
        byte *dst = new byte[out + SAFE_SPACE];
        int r = Kraken_Decompress(src, in, dst, out);
        unsigned int n = r == (int)out ? (unsigned int)out : 0;
        fwrite(&n, 4, 1, stdout);
        fwrite(dst, 1, n, stdout);
        fflush(stdout);
        delete[] src;
        delete[] dst;
    }
}
