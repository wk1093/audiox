#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>

namespace audiox::effects {

template <uint32_t N> struct FftPlan {
    static_assert(N >= 2 && (N & (N - 1)) == 0);
    std::array<uint32_t, N> reversed{};
    std::array<float, N - 1> real{}, imag{};

    FftPlan() {
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t bits = i, index = 0;
            for (uint32_t length = N; length > 1; length >>= 1) {
                index = (index << 1) | (bits & 1);
                bits >>= 1;
            }
            reversed[i] = index;
        }
        for (uint32_t length = 2; length <= N; length <<= 1) {
            const float angle = -2.0f * 3.14159265358979323846f / length;
            const float stepReal = std::cos(angle), stepImag = std::sin(angle);
            float re = 1, im = 0;
            for (uint32_t offset = 0; offset < length / 2; ++offset) {
                const uint32_t index = length / 2 - 1 + offset;
                real[index] = re;
                imag[index] = im;
                const float nextReal = re * stepReal - im * stepImag;
                im = re * stepImag + im * stepReal;
                re = nextReal;
            }
        }
    }

    void transform(float *data, bool inverse) const {
        for (uint32_t i = 0; i < N; ++i) {
            if (i < reversed[i]) {
                std::swap(data[i * 2], data[reversed[i] * 2]);
                std::swap(data[i * 2 + 1], data[reversed[i] * 2 + 1]);
            }
        }
        for (uint32_t length = 2; length <= N; length <<= 1) {
            for (uint32_t base = 0; base < N; base += length) {
                for (uint32_t offset = 0; offset < length / 2; ++offset) {
                    const uint32_t index = length / 2 - 1 + offset;
                    const float re = real[index], im = inverse ? -imag[index] : imag[index];
                    const uint32_t even = base + offset, odd = even + length / 2;
                    const float oddReal = data[odd * 2] * re - data[odd * 2 + 1] * im;
                    const float oddImag = data[odd * 2] * im + data[odd * 2 + 1] * re;
                    const float evenReal = data[even * 2], evenImag = data[even * 2 + 1];
                    data[even * 2] = evenReal + oddReal;
                    data[even * 2 + 1] = evenImag + oddImag;
                    data[odd * 2] = evenReal - oddReal;
                    data[odd * 2 + 1] = evenImag - oddImag;
                }
            }
        }
    }
};

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
