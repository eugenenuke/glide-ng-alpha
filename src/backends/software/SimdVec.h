#ifndef PROJECT_NG_BACKENDS_SOFTWARE_SIMDVEC_H_
#define PROJECT_NG_BACKENDS_SOFTWARE_SIMDVEC_H_

#include <immintrin.h>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>

// Forward declarations
class Simd8f;
class Simd8i;
class SimdMask;

#ifdef __AVX2__

// ============================================================================
// AVX2 Path: Wraps 256-bit AVX2 registers
// ============================================================================

class SimdMask {
public:
    __m256 v; // Wraps comparison mask as __m256 for float blending

    inline SimdMask() = default;
    inline explicit SimdMask(__m256 val) : v(val) {}
    inline explicit SimdMask(__m256i val) : v(_mm256_castsi256_ps(val)) {}

    static inline SimdMask True() {
        __m256i t = _mm256_set1_epi32(-1);
        return SimdMask(_mm256_castsi256_ps(t));
    }

    static inline SimdMask False() {
        return SimdMask(_mm256_setzero_ps());
    }

    inline SimdMask operator&(const SimdMask& other) const {
        return SimdMask(_mm256_and_ps(v, other.v));
    }

    inline SimdMask operator|(const SimdMask& other) const {
        return SimdMask(_mm256_or_ps(v, other.v));
    }

    inline SimdMask operator^(const SimdMask& other) const {
        return SimdMask(_mm256_xor_ps(v, other.v));
    }

    inline SimdMask operator!() const {
        __m256i t = _mm256_set1_epi32(-1);
        return SimdMask(_mm256_xor_ps(v, _mm256_castsi256_ps(t)));
    }

    inline SimdMask operator~() const {
        __m256i t = _mm256_set1_epi32(-1);
        return SimdMask(_mm256_xor_ps(v, _mm256_castsi256_ps(t)));
    }

    inline bool AllZero() const {
        return _mm256_movemask_ps(v) == 0;
    }

    inline bool AnyTrue() const {
        return _mm256_movemask_ps(v) != 0;
    }

    inline bool AllTrue() const {
        return _mm256_movemask_ps(v) == 0xFF;
    }

    inline __m256i AsInteger() const {
        return _mm256_castps_si256(v);
    }

    inline float operator[](size_t idx) const {
        alignas(32) float arr[8];
        _mm256_store_ps(arr, v);
        return arr[idx];
    }
};

class Simd8f {
public:
    __m256 v;

    inline Simd8f() = default;
    inline explicit Simd8f(__m256 val) : v(val) {}
    inline explicit Simd8f(float val) : v(_mm256_set1_ps(val)) {}
    inline Simd8f(float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7)
        : v(_mm256_setr_ps(f0, f1, f2, f3, f4, f5, f6, f7)) {}

    static inline Simd8f Load(const float* ptr) {
        return Simd8f(_mm256_loadu_ps(ptr));
    }

    static inline Simd8f LoadAligned(const float* ptr) {
        return Simd8f(_mm256_load_ps(ptr));
    }

    static inline void Store(float* ptr, const Simd8f& val) {
        _mm256_storeu_ps(ptr, val.v);
    }

    static inline void StoreAligned(float* ptr, const Simd8f& val) {
        _mm256_store_ps(ptr, val.v);
    }

    static inline Simd8f Load2x4(const float* ptr, size_t stride) {
        __m128 r0 = _mm_loadu_ps(ptr);
        __m128 r1 = _mm_loadu_ps(ptr + stride);
        __m256 val = _mm256_castps128_ps256(r0);
        val = _mm256_insertf128_ps(val, r1, 1);
        return Simd8f(val);
    }

    static inline void Store2x4(float* ptr, size_t stride, const Simd8f& val) {
        _mm_storeu_ps(ptr, _mm256_castps256_ps128(val.v));
        _mm_storeu_ps(ptr + stride, _mm256_extractf128_ps(val.v, 1));
    }

    static inline Simd8f Convert(const Simd8i& val);

    inline Simd8f operator+(const Simd8f& other) const {
        return Simd8f(_mm256_add_ps(v, other.v));
    }

    inline Simd8f operator-(const Simd8f& other) const {
        return Simd8f(_mm256_sub_ps(v, other.v));
    }

    inline Simd8f operator*(const Simd8f& other) const {
        return Simd8f(_mm256_mul_ps(v, other.v));
    }

    inline Simd8f operator/(const Simd8f& other) const {
        return Simd8f(_mm256_div_ps(v, other.v));
    }

    inline Simd8f& operator+=(const Simd8f& other) {
        v = _mm256_add_ps(v, other.v);
        return *this;
    }

    inline Simd8f& operator-=(const Simd8f& other) {
        v = _mm256_sub_ps(v, other.v);
        return *this;
    }

    inline Simd8f& operator*=(const Simd8f& other) {
        v = _mm256_mul_ps(v, other.v);
        return *this;
    }

    inline Simd8f& operator/=(const Simd8f& other) {
        v = _mm256_div_ps(v, other.v);
        return *this;
    }

    inline float operator[](size_t idx) const {
        alignas(32) float arr[8];
        _mm256_store_ps(arr, v);
        return arr[idx];
    }

    friend inline Simd8f Min(const Simd8f& a, const Simd8f& b) {
        return Simd8f(_mm256_min_ps(a.v, b.v));
    }

    friend inline Simd8f Max(const Simd8f& a, const Simd8f& b) {
        return Simd8f(_mm256_max_ps(a.v, b.v));
    }

    friend inline Simd8f Fma(const Simd8f& a, const Simd8f& b, const Simd8f& c) {
#ifdef __FMA__
        return Simd8f(_mm256_fmadd_ps(a.v, b.v, c.v));
#else
        return Simd8f(_mm256_add_ps(_mm256_mul_ps(a.v, b.v), c.v));
#endif
    }

    friend inline Simd8f Floor(const Simd8f& val) {
        return Simd8f(_mm256_round_ps(val.v, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
    }

    friend inline Simd8f Abs(const Simd8f& val) {
        __m256i mask = _mm256_set1_epi32(0x7FFFFFFF);
        return Simd8f(_mm256_and_ps(val.v, _mm256_castsi256_ps(mask)));
    }
};

inline SimdMask operator==(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ));
}
inline SimdMask operator!=(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm256_cmp_ps(a.v, b.v, _CMP_NEQ_OQ));
}
inline SimdMask operator<(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ));
}
inline SimdMask operator<=(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ));
}
inline SimdMask operator>(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ));
}
inline SimdMask operator>=(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ));
}

class Simd8i {
public:
    __m256i v;

    inline Simd8i() = default;
    inline explicit Simd8i(__m256i val) : v(val) {}
    inline explicit Simd8i(int32_t val) : v(_mm256_set1_epi32(val)) {}
    inline Simd8i(int32_t i0, int32_t i1, int32_t i2, int32_t i3, int32_t i4, int32_t i5, int32_t i6, int32_t i7)
        : v(_mm256_setr_epi32(i0, i1, i2, i3, i4, i5, i6, i7)) {}

    static inline Simd8i Load(const int32_t* ptr) {
        return Simd8i(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr)));
    }

    static inline Simd8i LoadAligned(const int32_t* ptr) {
        return Simd8i(_mm256_load_si256(reinterpret_cast<const __m256i*>(ptr)));
    }

    static inline void Store(int32_t* ptr, const Simd8i& val) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(ptr), val.v);
    }

    static inline void StoreAligned(int32_t* ptr, const Simd8i& val) {
        _mm256_store_si256(reinterpret_cast<__m256i*>(ptr), val.v);
    }

    static inline Simd8i Load2x4(const uint32_t* ptr, size_t stride) {
        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
        __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + stride));
        __m256i val = _mm256_castsi128_si256(r0);
        val = _mm256_insertf128_si256(val, r1, 1);
        return Simd8i(val);
    }

    static inline void Store2x4(uint32_t* ptr, size_t stride, const Simd8i& val) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr), _mm256_castsi256_si128(val.v));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr + stride), _mm256_extractf128_si256(val.v, 1));
    }

    static inline Simd8i Convert(const Simd8f& val) {
        return Simd8i(_mm256_cvttps_epi32(val.v));
    }

    inline Simd8i operator+(const Simd8i& other) const {
        return Simd8i(_mm256_add_epi32(v, other.v));
    }

    inline Simd8i operator-(const Simd8i& other) const {
        return Simd8i(_mm256_sub_epi32(v, other.v));
    }

    inline Simd8i operator*(const Simd8i& other) const {
        return Simd8i(_mm256_mullo_epi32(v, other.v));
    }

    inline Simd8i& operator+=(const Simd8i& other) {
        v = _mm256_add_epi32(v, other.v);
        return *this;
    }

    inline Simd8i& operator-=(const Simd8i& other) {
        v = _mm256_sub_epi32(v, other.v);
        return *this;
    }

    inline Simd8i& operator*=(const Simd8i& other) {
        v = _mm256_mullo_epi32(v, other.v);
        return *this;
    }

    inline Simd8i operator&(const Simd8i& other) const {
        return Simd8i(_mm256_and_si256(v, other.v));
    }

    inline Simd8i operator|(const Simd8i& other) const {
        return Simd8i(_mm256_or_si256(v, other.v));
    }

    inline Simd8i operator^(const Simd8i& other) const {
        return Simd8i(_mm256_xor_si256(v, other.v));
    }

    inline Simd8i operator~() const {
        return Simd8i(_mm256_xor_si256(v, _mm256_set1_epi32(-1)));
    }

    inline Simd8i& operator&=(const Simd8i& other) {
        v = _mm256_and_si256(v, other.v);
        return *this;
    }

    inline Simd8i& operator|=(const Simd8i& other) {
        v = _mm256_or_si256(v, other.v);
        return *this;
    }

    inline Simd8i& operator^=(const Simd8i& other) {
        v = _mm256_xor_si256(v, other.v);
        return *this;
    }

    inline Simd8i operator<<(int count) const {
        return Simd8i(_mm256_slli_epi32(v, count));
    }

    inline Simd8i operator>>(int count) const {
        return Simd8i(_mm256_srai_epi32(v, count));
    }

    inline Simd8i Srl(int count) const {
        return Simd8i(_mm256_srli_epi32(v, count));
    }

    inline Simd8i& operator<<=(int count) {
        v = _mm256_slli_epi32(v, count);
        return *this;
    }

    inline Simd8i& operator>>=(int count) {
        v = _mm256_srai_epi32(v, count);
        return *this;
    }

    inline int32_t operator[](size_t idx) const {
        alignas(32) int32_t arr[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(arr), v);
        return arr[idx];
    }

    friend inline Simd8i Min(const Simd8i& a, const Simd8i& b) {
        return Simd8i(_mm256_min_epi32(a.v, b.v));
    }

    friend inline Simd8i Max(const Simd8i& a, const Simd8i& b) {
        return Simd8i(_mm256_max_epi32(a.v, b.v));
    }

    static inline Simd8i Gather(const uint32_t* basePtr, const Simd8i& offsets) {
        return Simd8i(_mm256_i32gather_epi32(reinterpret_cast<const int*>(basePtr), offsets.v, 4));
    }
};

inline Simd8f Simd8f::Convert(const Simd8i& val) {
    return Simd8f(_mm256_cvtepi32_ps(val.v));
}

inline SimdMask operator==(const Simd8i& a, const Simd8i& b) {
    return SimdMask(_mm256_castsi256_ps(_mm256_cmpeq_epi32(a.v, b.v)));
}
inline SimdMask operator>(const Simd8i& a, const Simd8i& b) {
    return SimdMask(_mm256_castsi256_ps(_mm256_cmpgt_epi32(a.v, b.v)));
}
inline SimdMask operator<(const Simd8i& a, const Simd8i& b) {
    return SimdMask(_mm256_castsi256_ps(_mm256_cmpgt_epi32(b.v, a.v)));
}
inline SimdMask operator!=(const Simd8i& a, const Simd8i& b) {
    return !(a == b);
}
inline SimdMask operator<=(const Simd8i& a, const Simd8i& b) {
    return !(a > b);
}
inline SimdMask operator>=(const Simd8i& a, const Simd8i& b) {
    return !(a < b);
}

inline Simd8f Select(const SimdMask& mask, const Simd8f& trueVal, const Simd8f& falseVal) {
    return Simd8f(_mm256_blendv_ps(falseVal.v, trueVal.v, mask.v));
}

inline Simd8i Select(const SimdMask& mask, const Simd8i& trueVal, const Simd8i& falseVal) {
    return Simd8i(_mm256_castps_si256(_mm256_blendv_ps(
        _mm256_castsi256_ps(falseVal.v),
        _mm256_castsi256_ps(trueVal.v),
        mask.v
    )));
}

#else

// ============================================================================
// SSE4.2 Fallback Path: Wraps two 128-bit SSE registers
// ============================================================================

class SimdMask {
public:
    __m128 v0;
    __m128 v1;

    inline SimdMask() = default;
    inline explicit SimdMask(__m128 r0, __m128 r1) : v0(r0), v1(r1) {}
    inline explicit SimdMask(__m128i r0, __m128i r1) : v0(_mm_castsi128_ps(r0)), v1(_mm_castsi128_ps(r1)) {}

    static inline SimdMask True() {
        __m128i t = _mm_set1_epi32(-1);
        return SimdMask(_mm_castsi128_ps(t), _mm_castsi128_ps(t));
    }

    static inline SimdMask False() {
        return SimdMask(_mm_setzero_ps(), _mm_setzero_ps());
    }

    inline SimdMask operator&(const SimdMask& other) const {
        return SimdMask(_mm_and_ps(v0, other.v0), _mm_and_ps(v1, other.v1));
    }

    inline SimdMask operator|(const SimdMask& other) const {
        return SimdMask(_mm_or_ps(v0, other.v0), _mm_or_ps(v1, other.v1));
    }

    inline SimdMask operator^(const SimdMask& other) const {
        return SimdMask(_mm_xor_ps(v0, other.v0), _mm_xor_ps(v1, other.v1));
    }

    inline SimdMask operator!() const {
        __m128i t = _mm_set1_epi32(-1);
        return SimdMask(_mm_xor_ps(v0, _mm_castsi128_ps(t)), _mm_xor_ps(v1, _mm_castsi128_ps(t)));
    }

    inline SimdMask operator~() const {
        __m128i t = _mm_set1_epi32(-1);
        return SimdMask(_mm_xor_ps(v0, _mm_castsi128_ps(t)), _mm_xor_ps(v1, _mm_castsi128_ps(t)));
    }

    inline bool AllZero() const {
        return _mm_movemask_ps(v0) == 0 && _mm_movemask_ps(v1) == 0;
    }

    inline bool AnyTrue() const {
        return _mm_movemask_ps(v0) != 0 || _mm_movemask_ps(v1) != 0;
    }

    inline bool AllTrue() const {
        return _mm_movemask_ps(v0) == 0xF && _mm_movemask_ps(v1) == 0xF;
    }

    inline float operator[](size_t idx) const {
        alignas(16) float arr[8];
        _mm_store_ps(arr, v0);
        _mm_store_ps(arr + 4, v1);
        return arr[idx];
    }
};

class Simd8f {
public:
    __m128 v0;
    __m128 v1;

    inline Simd8f() = default;
    inline explicit Simd8f(__m128 r0, __m128 r1) : v0(r0), v1(r1) {}
    inline explicit Simd8f(float val) : v0(_mm_set1_ps(val)), v1(_mm_set1_ps(val)) {}
    inline Simd8f(float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7)
        : v0(_mm_setr_ps(f0, f1, f2, f3)), v1(_mm_setr_ps(f4, f5, f6, f7)) {}

    static inline Simd8f Load(const float* ptr) {
        return Simd8f(_mm_loadu_ps(ptr), _mm_loadu_ps(ptr + 4));
    }

    static inline Simd8f LoadAligned(const float* ptr) {
        return Simd8f(_mm_load_ps(ptr), _mm_load_ps(ptr + 4));
    }

    static inline void Store(float* ptr, const Simd8f& val) {
        _mm_storeu_ps(ptr, val.v0);
        _mm_storeu_ps(ptr + 4, val.v1);
    }

    static inline void StoreAligned(float* ptr, const Simd8f& val) {
        _mm_store_ps(ptr, val.v0);
        _mm_store_ps(ptr + 4, val.v1);
    }

    static inline Simd8f Load2x4(const float* ptr, size_t stride) {
        return Simd8f(_mm_loadu_ps(ptr), _mm_loadu_ps(ptr + stride));
    }

    static inline void Store2x4(float* ptr, size_t stride, const Simd8f& val) {
        _mm_storeu_ps(ptr, val.v0);
        _mm_storeu_ps(ptr + stride, val.v1);
    }

    static inline Simd8f Convert(const Simd8i& val);

    inline Simd8f operator+(const Simd8f& other) const {
        return Simd8f(_mm_add_ps(v0, other.v0), _mm_add_ps(v1, other.v1));
    }

    inline Simd8f operator-(const Simd8f& other) const {
        return Simd8f(_mm_sub_ps(v0, other.v0), _mm_sub_ps(v1, other.v1));
    }

    inline Simd8f operator*(const Simd8f& other) const {
        return Simd8f(_mm_mul_ps(v0, other.v0), _mm_mul_ps(v1, other.v1));
    }

    inline Simd8f operator/(const Simd8f& other) const {
        return Simd8f(_mm_div_ps(v0, other.v0), _mm_div_ps(v1, other.v1));
    }

    inline Simd8f& operator+=(const Simd8f& other) {
        v0 = _mm_add_ps(v0, other.v0);
        v1 = _mm_add_ps(v1, other.v1);
        return *this;
    }

    inline Simd8f& operator-=(const Simd8f& other) {
        v0 = _mm_sub_ps(v0, other.v0);
        v1 = _mm_sub_ps(v1, other.v1);
        return *this;
    }

    inline Simd8f& operator*=(const Simd8f& other) {
        v0 = _mm_mul_ps(v0, other.v0);
        v1 = _mm_mul_ps(v1, other.v1);
        return *this;
    }

    inline Simd8f& operator/=(const Simd8f& other) {
        v0 = _mm_div_ps(v0, other.v0);
        v1 = _mm_div_ps(v1, other.v1);
        return *this;
    }

    inline float operator[](size_t idx) const {
        alignas(16) float arr[8];
        _mm_store_ps(arr, v0);
        _mm_store_ps(arr + 4, v1);
        return arr[idx];
    }

    friend inline Simd8f Min(const Simd8f& a, const Simd8f& b) {
        return Simd8f(_mm_min_ps(a.v0, b.v0), _mm_min_ps(a.v1, b.v1));
    }

    friend inline Simd8f Max(const Simd8f& a, const Simd8f& b) {
        return Simd8f(_mm_max_ps(a.v0, b.v0), _mm_max_ps(a.v1, b.v1));
    }

    friend inline Simd8f Fma(const Simd8f& a, const Simd8f& b, const Simd8f& c) {
#ifdef __FMA__
        return Simd8f(_mm_fmadd_ps(a.v0, b.v0, c.v0), _mm_fmadd_ps(a.v1, b.v1, c.v1));
#else
        return Simd8f(_mm_add_ps(_mm_mul_ps(a.v0, b.v0), c.v0), _mm_add_ps(_mm_mul_ps(a.v1, b.v1), c.v1));
#endif
    }

    friend inline Simd8f Floor(const Simd8f& val) {
        return Simd8f(
            _mm_round_ps(val.v0, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC),
            _mm_round_ps(val.v1, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)
        );
    }

    friend inline Simd8f Abs(const Simd8f& val) {
        __m128i mask = _mm_set1_epi32(0x7FFFFFFF);
        return Simd8f(
            _mm_and_ps(val.v0, _mm_castsi128_ps(mask)),
            _mm_and_ps(val.v1, _mm_castsi128_ps(mask))
        );
    }
};

inline SimdMask operator==(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm_cmpeq_ps(a.v0, b.v0), _mm_cmpeq_ps(a.v1, b.v1));
}
inline SimdMask operator!=(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm_cmpneq_ps(a.v0, b.v0), _mm_cmpneq_ps(a.v1, b.v1));
}
inline SimdMask operator<(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm_cmplt_ps(a.v0, b.v0), _mm_cmplt_ps(a.v1, b.v1));
}
inline SimdMask operator<=(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm_cmple_ps(a.v0, b.v0), _mm_cmple_ps(a.v1, b.v1));
}
inline SimdMask operator>(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm_cmpgt_ps(a.v0, b.v0), _mm_cmpgt_ps(a.v1, b.v1));
}
inline SimdMask operator>=(const Simd8f& a, const Simd8f& b) {
    return SimdMask(_mm_cmpge_ps(a.v0, b.v0), _mm_cmpge_ps(a.v1, b.v1));
}

class Simd8i {
public:
    __m128i v0;
    __m128i v1;

    inline Simd8i() = default;
    inline explicit Simd8i(__m128i r0, __m128i r1) : v0(r0), v1(r1) {}
    inline explicit Simd8i(int32_t val) : v0(_mm_set1_epi32(val)), v1(_mm_set1_epi32(val)) {}
    inline Simd8i(int32_t i0, int32_t i1, int32_t i2, int32_t i3, int32_t i4, int32_t i5, int32_t i6, int32_t i7)
        : v0(_mm_setr_epi32(i0, i1, i2, i3)), v1(_mm_setr_epi32(i4, i5, i6, i7)) {}

    static inline Simd8i Load(const int32_t* ptr) {
        return Simd8i(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + 4)));
    }

    static inline Simd8i LoadAligned(const int32_t* ptr) {
        return Simd8i(_mm_load_si128(reinterpret_cast<const __m128i*>(ptr)), _mm_load_si128(reinterpret_cast<const __m128i*>(ptr + 4)));
    }

    static inline void Store(int32_t* ptr, const Simd8i& val) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr), val.v0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr + 4), val.v1);
    }

    static inline void StoreAligned(int32_t* ptr, const Simd8i& val) {
        _mm_store_si128(reinterpret_cast<__m128i*>(ptr), val.v0);
        _mm_store_si128(reinterpret_cast<__m128i*>(ptr + 4), val.v1);
    }

    static inline Simd8i Load2x4(const uint32_t* ptr, size_t stride) {
        return Simd8i(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + stride)));
    }

    static inline void Store2x4(uint32_t* ptr, size_t stride, const Simd8i& val) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr), val.v0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr + stride), val.v1);
    }

    static inline Simd8i Convert(const Simd8f& val) {
        return Simd8i(_mm_cvttps_epi32(val.v0), _mm_cvttps_epi32(val.v1));
    }

    inline Simd8i operator+(const Simd8i& other) const {
        return Simd8i(_mm_add_epi32(v0, other.v0), _mm_add_epi32(v1, other.v1));
    }

    inline Simd8i operator-(const Simd8i& other) const {
        return Simd8i(_mm_sub_epi32(v0, other.v0), _mm_sub_epi32(v1, other.v1));
    }

    inline Simd8i operator*(const Simd8i& other) const {
        return Simd8i(_mm_mullo_epi32(v0, other.v0), _mm_mullo_epi32(v1, other.v1));
    }

    inline Simd8i& operator+=(const Simd8i& other) {
        v0 = _mm_add_epi32(v0, other.v0);
        v1 = _mm_add_epi32(v1, other.v1);
        return *this;
    }

    inline Simd8i& operator-=(const Simd8i& other) {
        v0 = _mm_sub_epi32(v0, other.v0);
        v1 = _mm_sub_epi32(v1, other.v1);
        return *this;
    }

    inline Simd8i& operator*=(const Simd8i& other) {
        v0 = _mm_mullo_epi32(v0, other.v0);
        v1 = _mm_mullo_epi32(v1, other.v1);
        return *this;
    }

    inline Simd8i operator&(const Simd8i& other) const {
        return Simd8i(_mm_and_si128(v0, other.v0), _mm_and_si128(v1, other.v1));
    }

    inline Simd8i operator|(const Simd8i& other) const {
        return Simd8i(_mm_or_si128(v0, other.v0), _mm_or_si128(v1, other.v1));
    }

    inline Simd8i operator^(const Simd8i& other) const {
        return Simd8i(_mm_xor_si128(v0, other.v0), _mm_xor_si128(v1, other.v1));
    }

    inline Simd8i operator~() const {
        return Simd8i(_mm_xor_si128(v0, _mm_set1_epi32(-1)), _mm_xor_si128(v1, _mm_set1_epi32(-1)));
    }

    inline Simd8i& operator&=(const Simd8i& other) {
        v0 = _mm_and_si128(v0, other.v0);
        v1 = _mm_and_si128(v1, other.v1);
        return *this;
    }

    inline Simd8i& operator|=(const Simd8i& other) {
        v0 = _mm_or_si128(v0, other.v0);
        v1 = _mm_or_si128(v1, other.v1);
        return *this;
    }

    inline Simd8i& operator^=(const Simd8i& other) {
        v0 = _mm_xor_si128(v0, other.v0);
        v1 = _mm_xor_si128(v1, other.v1);
        return *this;
    }

    inline Simd8i operator<<(int count) const {
        return Simd8i(_mm_slli_epi32(v0, count), _mm_slli_epi32(v1, count));
    }

    inline Simd8i operator>>(int count) const {
        return Simd8i(_mm_srai_epi32(v0, count), _mm_srai_epi32(v1, count));
    }

    inline Simd8i Srl(int count) const {
        return Simd8i(_mm_srli_epi32(v0, count), _mm_srli_epi32(v1, count));
    }

    inline Simd8i& operator<<=(int count) {
        v0 = _mm_slli_epi32(v0, count);
        v1 = _mm_slli_epi32(v1, count);
        return *this;
    }

    inline Simd8i& operator>>=(int count) {
        v0 = _mm_srai_epi32(v0, count);
        v1 = _mm_srai_epi32(v1, count);
        return *this;
    }

    inline int32_t operator[](size_t idx) const {
        alignas(16) int32_t arr[8];
        _mm_store_si128(reinterpret_cast<__m128i*>(arr), v0);
        _mm_store_si128(reinterpret_cast<__m128i*>(arr + 4), v1);
        return arr[idx];
    }

    friend inline Simd8i Min(const Simd8i& a, const Simd8i& b) {
        return Simd8i(_mm_min_epi32(a.v0, b.v0), _mm_min_epi32(a.v1, b.v1));
    }

    friend inline Simd8i Max(const Simd8i& a, const Simd8i& b) {
        return Simd8i(_mm_max_epi32(a.v0, b.v0), _mm_max_epi32(a.v1, b.v1));
    }

    static inline Simd8i Gather(const uint32_t* basePtr, const Simd8i& offsets) {
        alignas(16) int32_t idx[8];
        _mm_store_si128(reinterpret_cast<__m128i*>(idx), offsets.v0);
        _mm_store_si128(reinterpret_cast<__m128i*>(idx + 4), offsets.v1);
        return Simd8i(
            _mm_setr_epi32(basePtr[idx[0]], basePtr[idx[1]], basePtr[idx[2]], basePtr[idx[3]]),
            _mm_setr_epi32(basePtr[idx[4]], basePtr[idx[5]], basePtr[idx[6]], basePtr[idx[7]])
        );
    }
};

inline Simd8f Simd8f::Convert(const Simd8i& val) {
    return Simd8f(_mm_cvtepi32_ps(val.v0), _mm_cvtepi32_ps(val.v1));
}

inline SimdMask operator==(const Simd8i& a, const Simd8i& b) {
    return SimdMask(_mm_cmpeq_epi32(a.v0, b.v0), _mm_cmpeq_epi32(a.v1, b.v1));
}
inline SimdMask operator>(const Simd8i& a, const Simd8i& b) {
    return SimdMask(_mm_cmpgt_epi32(a.v0, b.v0), _mm_cmpgt_epi32(a.v1, b.v1));
}
inline SimdMask operator<(const Simd8i& a, const Simd8i& b) {
    return SimdMask(_mm_cmpgt_epi32(b.v0, a.v0), _mm_cmpgt_epi32(b.v1, a.v1));
}
inline SimdMask operator!=(const Simd8i& a, const Simd8i& b) {
    return !(a == b);
}
inline SimdMask operator<=(const Simd8i& a, const Simd8i& b) {
    return !(a > b);
}
inline SimdMask operator>=(const Simd8i& a, const Simd8i& b) {
    return !(a < b);
}

inline Simd8f Select(const SimdMask& mask, const Simd8f& trueVal, const Simd8f& falseVal) {
    return Simd8f(_mm_blendv_ps(falseVal.v0, trueVal.v0, mask.v0), _mm_blendv_ps(falseVal.v1, trueVal.v1, mask.v1));
}

inline Simd8i Select(const SimdMask& mask, const Simd8i& trueVal, const Simd8i& falseVal) {
    return Simd8i(
        _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(falseVal.v0), _mm_castsi128_ps(trueVal.v0), mask.v0)),
        _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(falseVal.v1), _mm_castsi128_ps(trueVal.v1), mask.v1))
    );
}

#endif

// ============================================================================
// Precomputed Dither Vector Tables (Row-wise 2x4 block loading)
// ============================================================================
alignas(32) inline const int32_t kDitherMatrix4x4[4][8] = {
    { 0,  8,  2, 10, 12,  4, 14,  6}, // y % 4 == 0 (Row 0: 0,8,2,10; Row 1: 12,4,14,6)
    {12,  4, 14,  6,  3, 11,  1,  9}, // y % 4 == 1 (Row 0: 12,4,14,6; Row 1: 3,11,1,9)
    { 3, 11,  1,  9, 15,  7, 13,  5}, // y % 4 == 2 (Row 0: 3,11,1,9; Row 1: 15,7,13,5)
    {15,  7, 13,  5,  0,  8,  2, 10}  // y % 4 == 3 (Row 0: 15,7,13,5; Row 1: 0,8,2,10)
};

alignas(32) inline const int32_t kDitherMatrix2x2[4][8] = {
    { 2, 10,  2, 10, 14,  6, 14,  6}, // y % 4 == 0 (Row 0: 2,10,2,10; Row 1: 14,6,14,6)
    {14,  6, 14,  6,  2, 10,  2, 10}, // y % 4 == 1 (Row 0: 14,6,14,6; Row 1: 2,10,2,10)
    { 2, 10,  2, 10, 14,  6, 14,  6}, // y % 4 == 2 (Row 0: 2,10,2,10; Row 1: 14,6,14,6)
    {14,  6, 14,  6,  2, 10,  2, 10}  // y % 4 == 3 (Row 0: 14,6,14,6; Row 1: 2,10,2,10)
};

static inline Simd8i GetDitherVector4x4(int y) {
    return Simd8i::LoadAligned(&kDitherMatrix4x4[y & 3][0]);
}

static inline Simd8i GetDitherVector2x2(int y) {
    return Simd8i::LoadAligned(&kDitherMatrix2x2[y & 3][0]);
}

#endif  // PROJECT_NG_BACKENDS_SOFTWARE_SIMDVEC_H_
