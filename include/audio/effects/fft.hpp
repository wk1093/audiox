#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audiox::effects {

// Interleaved complex, radix-2 transform. Both directions are unnormalized.
inline void fftComplex(float *data, uint32_t n, bool inverse) {
    uint32_t j = 0;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        if (i < j) {
            std::swap(data[i * 2], data[j * 2]);
            std::swap(data[i * 2 + 1], data[j * 2 + 1]);
        }
        uint32_t bit = n >> 1;
        while (bit && j >= bit) {
            j -= bit;
            bit >>= 1;
        }
        j += bit;
    }
    for (uint32_t length = 2; length <= n; length <<= 1) {
        const float angle = (inverse ? 2.0f : -2.0f) * 3.14159265358979323846f / length;
        const float stepReal = std::cos(angle), stepImag = std::sin(angle);
        for (uint32_t base = 0; base < n; base += length) {
            float real = 1, imag = 0;
            for (uint32_t offset = 0; offset < length / 2; ++offset) {
                const uint32_t even = base + offset, odd = even + length / 2;
                const float oddReal = data[odd * 2] * real - data[odd * 2 + 1] * imag;
                const float oddImag = data[odd * 2] * imag + data[odd * 2 + 1] * real;
                const float evenReal = data[even * 2], evenImag = data[even * 2 + 1];
                data[even * 2] = evenReal + oddReal;
                data[even * 2 + 1] = evenImag + oddImag;
                data[odd * 2] = evenReal - oddReal;
                data[odd * 2 + 1] = evenImag - oddImag;
                const float nextReal = real * stepReal - imag * stepImag;
                imag = real * stepImag + imag * stepReal;
                real = nextReal;
            }
        }
    }
}

} // namespace audiox::effects
