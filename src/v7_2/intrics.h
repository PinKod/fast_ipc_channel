#ifndef INTRICS
#define INTRICS


#include <immintrin.h>
#include <string.h>

static inline void quick_memcpy_248(void* restrict dest, const void* restrict src, size_t n) {
    if (n == 248) {
        const char* s = (const char*)src;
        char* d = (char*)dest;
        
        __m256i y0 = _mm256_loadu_si256((const __m256i*)(s + 0));
        __m256i y1 = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i y2 = _mm256_loadu_si256((const __m256i*)(s + 64));
        __m256i y3 = _mm256_loadu_si256((const __m256i*)(s + 96));
        __m256i y4 = _mm256_loadu_si256((const __m256i*)(s + 128));
        __m256i y5 = _mm256_loadu_si256((const __m256i*)(s + 160));
        __m256i y6 = _mm256_loadu_si256((const __m256i*)(s + 192));
        
        __m256i y7 = _mm256_loadu_si256((const __m256i*)(s + 216));

        _mm256_storeu_si256((__m256i*)(d + 0), y0);
        _mm256_storeu_si256((__m256i*)(d + 32), y1);
        _mm256_storeu_si256((__m256i*)(d + 64), y2);
        _mm256_storeu_si256((__m256i*)(d + 96), y3);
        _mm256_storeu_si256((__m256i*)(d + 128), y4);
        _mm256_storeu_si256((__m256i*)(d + 160), y5);
        _mm256_storeu_si256((__m256i*)(d + 192), y6);
        _mm256_storeu_si256((__m256i*)(d + 216), y7);
    } else {
        memcpy(dest, src, n);
    }
}




#endif //INTRICS