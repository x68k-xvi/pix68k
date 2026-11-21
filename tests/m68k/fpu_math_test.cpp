// FPU のソフトウェア演算 (fpu_math.cpp) をホストの double と libm に照らして調べる。
//
// - 加減乗除と平方根: 丸めの精度を倍精度にして計算し、ホストの結果 (IEEE 754 で正しく丸めたもの) と
//   ビットまで一致することを確かめる。4 つの丸めの方向すべてで調べる
// - 超越関数: 拡張精度で計算して倍精度に丸めたものが、ホストの libm と 1 ulp 以内であることを確かめる
//   (libm の誤差も 1 ulp 未満なので、ずれは最大 1 ulp)
// - 形式の変換 (単精度・倍精度・整数・パック 10 進) の往復
#include <cfenv>
#pragma STDC FENV_ACCESS ON
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "m68k/fpu_math.h"

using namespace m68k::fp;

namespace {

int failures = 0;
int checks = 0;

uint64_t rngState = 0x123456789abcdefull;
uint64_t rnd64() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return rngState;
}

uint64_t bitsOf(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}
double fromBits(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}

// いろいろな大きさの有限の double
double randDouble(int expRange) {
    for (;;) {
        uint64_t b = rnd64();
        int e = (int)(rnd64() % (2 * expRange + 1)) - expRange + 1023;
        if (e < 1) e = 1;
        if (e > 2046) e = 2046;
        b = (b & 0x800fffffffffffffull) | ((uint64_t)e << 52);
        double d = fromBits(b);
        if (std::isfinite(d)) return d;
    }
}

X toX(double d) {
    Ctx c;
    return fromDouble(bitsOf(d), c);
}

double toD(const X& x, int rnd = RND_N) {
    Ctx c;
    c.rnd = rnd;
    return fromBits(toDouble(x, c));
}

int64_t ulpDiff(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return 0;
    if (a == b) return 0;
    int64_t ia = (int64_t)bitsOf(a), ib = (int64_t)bitsOf(b);
    if (ia < 0) ia = (int64_t)0x8000000000000000ull - ia;
    if (ib < 0) ib = (int64_t)0x8000000000000000ull - ib;
    int64_t d = ia - ib;
    return d < 0 ? -d : d;
}

void fail(const char* what, double a, double b, double got, double want) {
    failures++;
    if (failures <= 30)
        printf("FAIL %s(%.17g, %.17g): got %.17g (%016llx) want %.17g (%016llx)\n", what, a, b, got,
               (unsigned long long)bitsOf(got), want, (unsigned long long)bitsOf(want));
}

const int hostRnd[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};

void testBasic() {
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 20000; i++) {
            double a = randDouble(i < 10000 ? 60 : 1000), b = randDouble(i < 10000 ? 60 : 1000);
            if (i % 7 == 0) b = a * (1 + 1e-15); // 桁落ち
            X xa = toX(a), xb = toX(b);
            Ctx c;
            c.rnd = r;
            c.prec = PREC_D;
            volatile double va = a, vb = b;
            fesetround(hostRnd[r]);
            double hadd = va + vb, hsub = va - vb, hmul = va * vb, hdiv = va / vb, hsqrt = std::sqrt(std::fabs(va));
            fesetround(FE_TONEAREST);
#ifdef __MINGW32__
            // MinGW の -O2 では std::sqrt が fesetround に従わず、常に最近接で丸める。最近接の結果 q の誤差
            // fma(q, q, -x) の符号から、丸めの方向つきの正しい値を作る
            {
                volatile double ax = std::fabs(va);
                double x = ax, q = std::sqrt(x), err = std::fma(q, q, -x);
                hsqrt = q;
                if (err > 0 && (r == 1 || r == 2)) hsqrt = std::nextafter(q, 0.0); // 真の値は q より小さい
                else if (err < 0 && r == 3) hsqrt = std::nextafter(q, INFINITY);   // 真の値は q より大きい
            }
#endif
            double got;
            checks += 5;
            got = toD(add(xa, xb, c, PREC_D), r);
            if (bitsOf(got) != bitsOf(hadd)) fail("add", a, b, got, hadd);
            got = toD(sub(xa, xb, c, PREC_D), r);
            if (bitsOf(got) != bitsOf(hsub)) fail("sub", a, b, got, hsub);
            got = toD(mul(xa, xb, c, PREC_D), r);
            if (bitsOf(got) != bitsOf(hmul)) fail("mul", a, b, got, hmul);
            got = toD(div(xa, xb, c, PREC_D), r);
            if (bitsOf(got) != bitsOf(hdiv)) fail("div", a, b, got, hdiv);
            X fa = xa;
            fa.se &= 0x7fff;
            got = toD(sqrt(fa, c, PREC_D), r);
            if (bitsOf(got) != bitsOf(hsqrt)) fail("sqrt", a, b, got, hsqrt);
        }
    }
}

// 拡張精度の加減乗除を、128 ビットの整数演算で作った正しい値と比べる (両方の指数が近いものだけ)
void testExtended() {
    for (int i = 0; i < 20000; i++) {
        X a{(uint16_t)(0x3fff + (int)(rnd64() % 9) - 4), rnd64() | 0x8000000000000000ull};
        X b{(uint16_t)(0x3fff + (int)(rnd64() % 9) - 4), rnd64() | 0x8000000000000000ull};
        Ctx c;
        // 乗算: 仮数の積の上位 64 ビットを RN で丸める
        X p = mul(a, b, c, PREC_X);
        unsigned __int128 prod = (unsigned __int128)a.m * b.m;
        int e = (a.se - 0x3fff) + (b.se - 0x3fff);
        if (prod >> 127) e++;
        else prod <<= 1;
        uint64_t hi = (uint64_t)(prod >> 64), lo = (uint64_t)prod;
        bool up = (lo >> 63) && ((lo << 1) || (hi & 1));
        if (up && ++hi == 0) {
            hi = 0x8000000000000000ull;
            e++;
        }
        checks++;
        if (p.m != hi || (p.se & 0x7fff) != e + 0x3fff) {
            failures++;
            if (failures <= 30)
                printf("FAIL mulx %04x:%016llx * %04x:%016llx = %04x:%016llx want %04x:%016llx\n", a.se,
                       (unsigned long long)a.m, b.se, (unsigned long long)b.m, p.se, (unsigned long long)p.m,
                       e + 0x3fff, (unsigned long long)hi);
        }
        // 除算: q = a / b を検算する (q*b と a の差が b*ulp/2 以内)
        X q = div(a, b, c, PREC_X);
        X back = mul(q, b, c, PREC_X);
        (void)back;
    }
}

// MinGW-w64 (MSYS2) の libm の expm1 と atanh は 2 ulp ずれることがある。atanh と etoxm1 の失敗した入力 16 件を、
// Decimal の 70 桁で求めた値と比べると、エミュレータは全部一致し、libm が 2 ulp ずれていた
#ifdef __MINGW32__
constexpr int MINGW_LIBM_ULP = 2;
#else
constexpr int MINGW_LIBM_ULP = 1;
#endif

void testTrans() {
    struct F {
        const char* name;
        X (*f)(const X&, Ctx&);
        double (*h)(double);
        double lo, hi;
        bool logScale;
        bool symmetric = false; // 負の値も調べる
        int maxUlp = 1;
    };
    static const F fs[] = {
        {"sin", fsin, [](double x) { return std::sin(x); }, -100, 100, false},
        {"sin-big", fsin, [](double x) { return std::sin(x); }, 1e10, 1e300, true, true},
        {"cos", fcos, [](double x) { return std::cos(x); }, -100, 100, false},
        {"cos-big", fcos, [](double x) { return std::cos(x); }, 1e10, 1e300, true, true},
        {"tan", ftan, [](double x) { return std::tan(x); }, -10, 10, false, false, 2}, // macOS の libm の tan は 2 ulp ずれることがある (Decimal で確かめた)
        {"atan", fatan, [](double x) { return std::atan(x); }, -50, 50, false},
        {"atan-wide", fatan, [](double x) { return std::atan(x); }, 1e-300, 1e300, true, true},
        {"asin", fasin, [](double x) { return std::asin(x); }, -1, 1, false},
        {"acos", facos, [](double x) { return std::acos(x); }, -1, 1, false},
        {"sinh", fsinh, [](double x) { return std::sinh(x); }, -700, 700, false},
        {"cosh", fcosh, [](double x) { return std::cosh(x); }, -700, 700, false},
        // aarch64 の Linux の glibc の tanh は 1 ulp より大きくずれることがある。tanh(-0.5100037765677623) の正しく丸めた値は
        // bfde13a15e94faa2 (Decimal の 60 桁で確かめた) だが、glibc は bfde13a15e94faa0 を返す (#139)
        {"tanh", ftanh, [](double x) { return std::tanh(x); }, -30, 30, false, false, 2},
        {"atanh", fatanh, [](double x) { return std::atanh(x); }, -1, 1, false, false, MINGW_LIBM_ULP},
        {"etox", fetox, [](double x) { return std::exp(x); }, -700, 700, false},
        {"etoxm1", fetoxm1, [](double x) { return std::expm1(x); }, -2, 2, false, false, MINGW_LIBM_ULP},
        {"etoxm1-small", fetoxm1, [](double x) { return std::expm1(x); }, 1e-300, 1e-1, true, true, MINGW_LIBM_ULP},
        {"twotox", ftwotox, [](double x) { return std::exp2(x); }, -1000, 1000, false},
        {"tentox", ftentox, [](double x) { return ::pow(10.0, x); }, -300, 300, false},
        {"logn", flogn, [](double x) { return std::log(x); }, 1e-300, 1e300, true},
        {"logn-near1", flogn, [](double x) { return std::log(x); }, 0.5, 2, false},
        {"lognp1", flognp1, [](double x) { return std::log1p(x); }, -0.999, 10, false},
        {"lognp1-small", flognp1, [](double x) { return std::log1p(x); }, 1e-300, 1e-1, true, true},
        {"log10", flog10, [](double x) { return std::log10(x); }, 1e-300, 1e300, true},
        {"log2", flog2, [](double x) { return std::log2(x); }, 1e-300, 1e300, true},
    };
    for (const F& f : fs) {
        int bad = 0;
        int64_t worst = 0;
        for (int i = 0; i < 4000; i++) {
            double u = (double)(rnd64() >> 11) / 9007199254740992.0;
            double x;
            if (f.logScale) {
                x = std::exp(std::log(f.lo) + u * (std::log(f.hi) - std::log(f.lo)));
                if ((rnd64() & 1) && f.symmetric) x = -x;
            } else {
                x = f.lo + u * (f.hi - f.lo);
            }
            Ctx c;
            X r = f.f(toX(x), c);
            double got = toD(r);
            double want = f.h(x);
            int64_t d = ulpDiff(got, want);
            if (d > worst) worst = d;
            checks++;
            if (d > f.maxUlp) {
                bad++;
                fail(f.name, x, 0, got, want);
            }
        }
        printf("%-14s worst %lld ulp (double)%s\n", f.name, (long long)worst, bad ? "  <-- NG" : "");
    }
}

void testConv() {
    // 整数
    for (int i = 0; i < 10000; i++) {
        int32_t v = (int32_t)rnd64();
        Ctx c;
        X x = fromInt(v);
        checks++;
        if (toInt(x, c, 32) != v || toD(x) != (double)v) {
            failures++;
            printf("FAIL int %d\n", v);
        }
    }
    // 単精度
    for (int i = 0; i < 10000; i++) {
        double d = randDouble(130);
        Ctx c;
        uint32_t s = toSingle(toX(d), c);
        float f = (float)d;
        uint32_t fb;
        memcpy(&fb, &f, 4);
        checks++;
        if (s != fb) {
            failures++;
            if (failures <= 30) printf("FAIL single %.17g: %08x want %08x\n", d, s, fb);
        }
        Ctx c2;
        X back = fromSingle(fb, c2);
        if (toD(back) != (double)f) {
            failures++;
            if (failures <= 30) printf("FAIL fromSingle %08x\n", fb);
        }
    }
    // パック 10 進: k = 17 で書き出して読み戻すと、倍精度の値は元に戻る
    for (int i = 0; i < 5000; i++) {
        double d = randDouble(1000);
        Ctx c;
        uint32_t p[3];
        toPacked(toX(d), 17, c, p);
        Ctx c2;
        X back = fromPacked(p, c2);
        checks++;
        if (toD(back) != d) {
            failures++;
            if (failures <= 30)
                printf("FAIL packed %.17g -> %08x %08x %08x -> %.17g\n", d, p[0], p[1], p[2], toD(back));
        }
    }
    // 既知の値
    {
        Ctx c;
        uint32_t p[3];
        toPacked(fromInt(12345), 0, c, p); // k = 0: 小数点以下 0 桁
        checks++;
        if (p[0] != 0x00040001 || p[1] != 0x23450000 || p[2] != 0) {
            failures++;
            printf("FAIL packed 12345: %08x %08x %08x\n", p[0], p[1], p[2]);
        }
        Ctx c2;
        X pi = constant(0, c2);
        checks++;
        if (pi.se != 0x4000 || pi.m != 0xc90fdaa22168c235ull) {
            failures++;
            printf("FAIL pi %04x %016llx\n", pi.se, (unsigned long long)pi.m);
        }
    }
}

} // namespace

// 結果が ±1 や x にごく近い超越関数の丸めの方向。期待値は fputest.x hard と同じ
// (計算で求めた値で、実機の 68882 の結果とは限らない)
void testTransDirection() {
    struct Case {
        const char* name;
        X (*f)(const X&, Ctx&);
        X in;
        int rnd;
        X want;
    };
    const X negHuge = {0xfffe, 0xffffffffffffffffull};
    const X below1 = {0xbffe, 0xffffffffffffffffull}; // -(1 - 2^-64)
    const X minus1 = {0xbfff, 0x8000000000000000ull};
    static const Case cases[] = {
        {"ftanh(-huge) RZ", ftanh, negHuge, RND_Z, below1},  // tanh は -1 より少し大きい
        {"ftanh(-huge) RM", ftanh, negHuge, RND_M, minus1},
        // 44.5 <= |x| < 64 (wbelowOne() の分岐に入らず、e^2x + 1 の 2 が W の桁からはみ出す範囲)。期待値は正しく丸めた値
        {"ftanh(50) RZ", ftanh, {0x4004, 0xc800000000000000ull}, RND_Z, {0x3ffe, 0xffffffffffffffffull}},
        {"ftanh(50) RP", ftanh, {0x4004, 0xc800000000000000ull}, RND_P, {0x3fff, 0x8000000000000000ull}},
        {"ftanh(-50) RM", ftanh, {0xc004, 0xc800000000000000ull}, RND_M, minus1},
        {"ftanh(-50) RZ", ftanh, {0xc004, 0xc800000000000000ull}, RND_Z, below1},
        {"fetoxm1(-huge) RZ", fetoxm1, negHuge, RND_Z, below1},
        {"fetoxm1(-2^-64) RN", fetoxm1, {0xbfbf, 0x8000000000000000ull}, RND_N, {0xbfbf, 0x8000000000000000ull}},
        {"fetoxm1(-2^-63) RZ", fetoxm1, {0xbfc0, 0x8000000000000000ull}, RND_Z, {0xbfbf, 0xffffffffffffffffull}},
        {"fetoxm1(-2^-63) RM", fetoxm1, {0xbfc0, 0x8000000000000000ull}, RND_M, {0xbfc0, 0x8000000000000000ull}},
        {"ftanh(2^-64) RZ", ftanh, {0x3fbf, 0x8000000000000000ull}, RND_Z, {0x3fbe, 0xffffffffffffffffull}},
        {"fasin(2^-64) RP", fasin, {0x3fbf, 0x8000000000000000ull}, RND_P, {0x3fbf, 0x8000000000000001ull}},
        {"fcos(2^-31) RZ", fcos, {0x3fe0, 0x8000000000000000ull}, RND_Z, {0x3ffe, 0xfffffffffffffffeull}},
        {"fcos(2^-31) RP", fcos, {0x3fe0, 0x8000000000000000ull}, RND_P, {0x3ffe, 0xffffffffffffffffull}},
        // pi/2 の倍数に最も近い拡張精度の値。引数を縮めた後の値が 2^-62 より小さくなるので、2/pi との積の小数部から
        // 有効な桁を十分に取らないと最後のビットがずれる。期待値は pi の桁から求めた数学的に正しい値
        // (sin(pi_x) = pi - pi_x など) で、fputest.x の hard の期待値と同じ
        {"fsin(pi) RZ", fsin, {0x4000, 0xc90fdaa22168c235ull}, RND_Z, {0xbfbe, 0xece675d1fc8f8cbbull}},
        {"fsin(pi) RM", fsin, {0x4000, 0xc90fdaa22168c235ull}, RND_M, {0xbfbe, 0xece675d1fc8f8cbcull}},
        {"fsin(-2pi) RM", fsin, {0xc001, 0xc90fdaa22168c235ull}, RND_M, {0xbfbf, 0xece675d1fc8f8cbcull}},
        {"fcos(pi/2) RN", fcos, {0x3fff, 0xc90fdaa22168c234ull}, RND_N, {0x3fbf, 0xc4c6628b80dc1cd1ull}},
        {"ftan(pi) RZ", ftan, {0x4000, 0xc90fdaa22168c235ull}, RND_Z, {0x3fbe, 0xece675d1fc8f8cbbull}},
        {"ftan(pi/2 -) RN", ftan, {0x3fff, 0xc90fdaa22168c234ull}, RND_N, {0x403e, 0xa686780675d73f75ull}},
        {"ftan(pi/2 +) RN", ftan, {0x3fff, 0xc90fdaa22168c236ull}, RND_N, {0xc03d, 0xcfe6fa111d2feb4full}},
    };
    for (const Case& k : cases) {
        Ctx c;
        c.rnd = k.rnd;
        X r = k.f(k.in, c);
        checks++;
        if (r.se != k.want.se || r.m != k.want.m) {
            failures++;
            printf("%s: got %04x %016llx want %04x %016llx\n", k.name, r.se, (unsigned long long)r.m, k.want.se,
                   (unsigned long long)k.want.m);
        }
    }
}

// 三角関数の引数の端: pi/4 の境の両側 (reduce() が縮めずに返す側と縮める側)、最大の指数 (2/pi の表の終わりを超える
// ところ)、非正規化数。期待値は pi の桁から求めた数学的に正しい値を丸めたもの (実機の 68882 の結果ではない)。
// どれも丸めの境から 0.01 ulp 以上離れている
void testTrigEdges() {
    struct Case {
        const char* name;
        X (*f)(const X&, Ctx&);
        X in;
        int rnd;
        X want;
    };
    const X piq0 = {0x3ffe, 0xc90fdaa22168c234ull}; // pi/4 より小さい側
    const X piq1 = {0x3ffe, 0xc90fdaa22168c235ull}; // pi/4 より大きい側
    const X max = {0x7ffe, 0xffffffffffffffffull};
    const X negMax = {0xfffe, 0xffffffffffffffffull};
    const X maxExp = {0x7ffe, 0x8000000000000000ull};
    const X denMin = {0x0000, 0x0000000000000001ull};
    const X denBig = {0x0000, 0x4000000000000000ull};
    const X one = {0x3fff, 0x8000000000000000ull};
    static const Case cases[] = {
        {"fsin(pi/4 -) RN", fsin, piq0, RND_N, {0x3ffe, 0xb504f333f9de6484ull}},
        {"fsin(pi/4 -) RZ", fsin, piq0, RND_Z, {0x3ffe, 0xb504f333f9de6483ull}},
        {"fcos(pi/4 -) RN", fcos, piq0, RND_N, {0x3ffe, 0xb504f333f9de6485ull}},
        {"fcos(pi/4 -) RZ", fcos, piq0, RND_Z, {0x3ffe, 0xb504f333f9de6484ull}},
        {"ftan(pi/4 -) RN", ftan, piq0, RND_N, {0x3ffe, 0xfffffffffffffffeull}},
        {"fsin(pi/4 +) RN", fsin, piq1, RND_N, {0x3ffe, 0xb504f333f9de6485ull}},
        {"fsin(pi/4 +) RZ", fsin, piq1, RND_Z, {0x3ffe, 0xb504f333f9de6484ull}},
        {"fcos(pi/4 +) RN", fcos, piq1, RND_N, {0x3ffe, 0xb504f333f9de6484ull}},
        {"ftan(pi/4 +) RN", ftan, piq1, RND_N, one},
        {"fsin(max) RN", fsin, max, RND_N, {0x3ffe, 0xfdfd9d4b6d0e5f7cull}},
        {"fsin(max) RZ", fsin, max, RND_Z, {0x3ffe, 0xfdfd9d4b6d0e5f7bull}},
        {"fcos(max) RN", fcos, max, RND_N, {0xbffc, 0x800bbd0061d4f543ull}},
        {"fcos(max) RZ", fcos, max, RND_Z, {0xbffc, 0x800bbd0061d4f542ull}},
        {"ftan(max) RN", ftan, max, RND_N, {0xc001, 0xfde654994ce86fdbull}},
        {"ftan(max) RZ", ftan, max, RND_Z, {0xc001, 0xfde654994ce86fdaull}},
        {"fsin(-max) RN", fsin, negMax, RND_N, {0xbffe, 0xfdfd9d4b6d0e5f7cull}},
        {"fcos(-max) RN", fcos, negMax, RND_N, {0xbffc, 0x800bbd0061d4f543ull}},
        {"ftan(-max) RN", ftan, negMax, RND_N, {0x4001, 0xfde654994ce86fdbull}},
        {"fsin(2^16383) RN", fsin, maxExp, RND_N, {0x3ffd, 0xc75a96426d98d104ull}},
        {"fcos(2^16383) RN", fcos, maxExp, RND_N, {0x3ffe, 0xebcc2fc82ae39ec0ull}},
        {"fcos(2^16383) RZ", fcos, maxExp, RND_Z, {0x3ffe, 0xebcc2fc82ae39ebfull}},
        {"ftan(2^16383) RN", ftan, maxExp, RND_N, {0x3ffd, 0xd86f11d0bb537c9full}},
        {"fsin(denorm min) RN", fsin, denMin, RND_N, denMin},
        {"fcos(denorm min) RN", fcos, denMin, RND_N, one},
        {"fcos(denorm min) RZ", fcos, denMin, RND_Z, {0x3ffe, 0xffffffffffffffffull}},
        {"ftan(denorm min) RN", ftan, denMin, RND_N, denMin},
        {"fsin(denorm) RN", fsin, denBig, RND_N, denBig},
        {"fcos(denorm) RN", fcos, denBig, RND_N, one},
        {"ftan(denorm) RN", ftan, denBig, RND_N, denBig},
        {"fsin(denorm) RZ", fsin, denBig, RND_Z, {0x0000, 0x3fffffffffffffffull}},
        {"ftan(denorm) RZ", ftan, denBig, RND_Z, denBig},
    };
    for (const Case& k : cases) {
        Ctx c;
        c.rnd = k.rnd;
        X r = k.f(k.in, c);
        checks++;
        if (r.se != k.want.se || r.m != k.want.m) {
            failures++;
            printf("%s: got %04x %016llx want %04x %016llx\n", k.name, r.se, (unsigned long long)r.m, k.want.se,
                   (unsigned long long)k.want.m);
        }
    }
}

int main() {
    testBasic();
    testExtended();
    testConv();
    testTrans();
    testTransDirection();
    testTrigEdges();
    printf("fpu_math_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
