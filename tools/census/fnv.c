#include <stdint.h>
#include <stddef.h>
uint64_t fnv1a64(const uint8_t *b, size_t n){uint64_t h=1469598103934665603ull;while(n--){h^=*b++;h*=1099511628211ull;}return h;}
