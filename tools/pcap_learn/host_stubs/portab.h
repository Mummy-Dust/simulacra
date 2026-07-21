/* MSVC host shim, force-included (/FIportab.h) into every TU. */
#define __attribute__(x)
/* MSVC lacks GCC's __builtin_popcount (roster.c uses it for addr bit-balance). */
static int __builtin_popcount(unsigned x){ int c=0; while(x){ c+=x&1u; x>>=1; } return c; }
