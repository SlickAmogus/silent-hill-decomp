/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <libgte.h>

extern void OuterProduct12(VECTOR* v0, VECTOR* v1, VECTOR* v2);

static void expect(VECTOR v, int x, int y, int z)
{
    assert(v.vx == x);
    assert(v.vy == y);
    assert(v.vz == z);
}

int main(void)
{
    VECTOR x = { 4096, 0, 0, 0 };
    VECTOR y = { 0, 4096, 0, 0 };
    VECTOR out = { 123, 456, 789, 42 };
    VECTOR a = { 2048, -4096, 1024, 0 };
    VECTOR b = { -1024, 2048, 4096, 0 };

    OuterProduct12(&x, &y, &out);
    expect(out, 0, 0, 4096);
    assert(out.pad == 42);
    OuterProduct12(&y, &x, &out);
    expect(out, 0, 0, -4096);
    OuterProduct12(&x, &x, &out);
    expect(out, 0, 0, 0);
    OuterProduct12(&a, &b, &out);
    expect(out, -4608, -2304, 0);
    OuterProduct12(&a, &b, &a);
    expect(a, -4608, -2304, 0);
    a = (VECTOR){ 2048, -4096, 1024, 0 };
    OuterProduct12(&a, &b, &b);
    expect(b, -4608, -2304, 0);

    /* Negative fractional products must retain Q12 arithmetic-shift rounding. */
    a = (VECTOR){ 1, 0, 0, 0 };
    b = (VECTOR){ 0, 0, 1, 0 };
    OuterProduct12(&a, &b, &out);
    expect(out, 0, -1, 0);
    return 0;
}
