// 32-bit builds only (see CMakeLists.txt). MinHook's hde64.c is plain C over a byte buffer but
// compiles to nothing unless the HOST is x64. The x64 code readers need it in a 32-bit build too
// (they read x64 images as data), so it is compiled here with its own guard satisfied.
#define __x86_64__ 1
#include "../../../third_party/minhook/src/hde/hde64.c"
