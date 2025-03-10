// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// DSLX standard library routines.

// TODO(tedhong): 2023-03-15 - Convert to a macro to support getting size of
//                             arbitrary types.
// Returns the number of bits (sizeof) of the type of the given bits value.
pub fn sizeof_signed<N: u32>(x: sN[N]) -> u32 { N }

pub fn sizeof_unsigned<N: u32>(x: uN[N]) -> u32 { N }

#[test]
fn sizeof_signed_test() {
    assert_eq(u32:0, sizeof_signed(sN[0]:0));
    assert_eq(u32:1, sizeof_signed(sN[1]:0));
    assert_eq(u32:2, sizeof_signed(sN[2]:0));

    //TODO(tedhong): 2023-03-15 - update frontend to support below.
    //assert_eq(u32:0xffffffff, sizeof_signed(uN[0xffffffff]:0));
}

#[test]
fn sizeof_unsigned_test() {
    assert_eq(u32:0, sizeof_unsigned(uN[0]:0));
    assert_eq(u32:1, sizeof_unsigned(uN[1]:0));
    assert_eq(u32:2, sizeof_unsigned(uN[2]:0));

    //TODO(tedhong): 2023-03-15 - update frontend to support below.
    //assert_eq(u32:0xffffffff, sizeof_unsigned(uN[0xffffffff]:0));
}

#[test]
fn use_sizeof_test() {
    let x = uN[32]:0xffffffff;
    let y: uN[sizeof_unsigned(x) + u32:2] = x as uN[sizeof_unsigned(x) + u32:2];
    assert_eq(y, uN[34]:0xffffffff);
}

// Returns the maximum signed value contained in N bits.
pub fn signed_max_value<N: u32, N_MINUS_ONE: u32 = {N - u32:1}>() -> sN[N] {
    ((sN[N]:1 << N_MINUS_ONE) - sN[N]:1) as sN[N]
}

#[test]
fn signed_max_value_test() {
    assert_eq(s8:0x7f, signed_max_value<u32:8>());
    assert_eq(s16:0x7fff, signed_max_value<u32:16>());
    assert_eq(s17:0xffff, signed_max_value<u32:17>());
    assert_eq(s32:0x7fffffff, signed_max_value<u32:32>());
}

// Returns the minimum signed value contained in N bits.
pub fn signed_min_value<N: u32, N_MINUS_ONE: u32 = {N - u32:1}>() -> sN[N] {
    (uN[N]:1 << N_MINUS_ONE) as sN[N]
}

#[test]
fn signed_min_value_test() {
    assert_eq(s8:-128, signed_min_value<u32:8>());
    assert_eq(s16:-32768, signed_min_value<u32:16>());
    assert_eq(s17:-65536, signed_min_value<u32:17>());
}

pub fn unsigned_min_value<N: u32>() -> uN[N] { zero!<uN[N]>() }

#[test]
fn unsigned_min_value_test() {
    assert_eq(u8:0, unsigned_min_value<u32:8>());
    assert_eq(u16:0, unsigned_min_value<u32:16>());
    assert_eq(u17:0, unsigned_min_value<u32:17>());
}

// Returns the maximum unsigned value contained in N bits.
pub fn unsigned_max_value<N: u32, N_PLUS_ONE: u32 = {N + u32:1}>() -> uN[N] {
    ((uN[N_PLUS_ONE]:1 << N) - uN[N_PLUS_ONE]:1) as uN[N]
}

#[test]
fn unsigned_max_value_test() {
    assert_eq(u8:0xff, unsigned_max_value<u32:8>());
    assert_eq(u16:0xffff, unsigned_max_value<u32:16>());
    assert_eq(u17:0x1ffff, unsigned_max_value<u32:17>());
    assert_eq(u32:0xffffffff, unsigned_max_value<u32:32>());
}

// Returns the maximum of two signed integers.
pub fn smax<N: u32>(x: sN[N], y: sN[N]) -> sN[N] { if x > y { x } else { y } }

#[test]
fn smax_test() {
    assert_eq(s2:0, smax(s2:0, s2:0));
    assert_eq(s2:1, smax(s2:-1, s2:1));
    assert_eq(s7:-3, smax(s7:-3, s7:-6));
}

// Returns the maximum of two unsigned integers.
pub fn umax<N: u32>(x: uN[N], y: uN[N]) -> uN[N] { if x > y { x } else { y } }

#[test]
fn umax_test() {
    assert_eq(u1:1, umax(u1:1, u1:0));
    assert_eq(u1:1, umax(u1:1, u1:1));
    assert_eq(u2:3, umax(u2:3, u2:2));
}

// Returns the maximum of two signed integers.
pub fn smin<N: u32>(x: sN[N], y: sN[N]) -> sN[N] { if x < y { x } else { y } }

#[test]
fn smin_test() {
    assert_eq(s1:0, smin(s1:0, s1:0));
    assert_eq(s1:-1, smin(s1:0, s1:1));
    assert_eq(s1:-1, smin(s1:1, s1:0));
    assert_eq(s1:-1, smin(s1:1, s1:1));

    assert_eq(s2:-2, smin(s2:0, s2:-2));
    assert_eq(s2:-1, smin(s2:0, s2:-1));
    assert_eq(s2:0, smin(s2:0, s2:0));
    assert_eq(s2:0, smin(s2:0, s2:1));

    assert_eq(s2:-2, smin(s2:1, s2:-2));
    assert_eq(s2:-1, smin(s2:1, s2:-1));
    assert_eq(s2:0, smin(s2:1, s2:0));
    assert_eq(s2:1, smin(s2:1, s2:1));

    assert_eq(s2:-2, smin(s2:-2, s2:-2));
    assert_eq(s2:-2, smin(s2:-2, s2:-1));
    assert_eq(s2:-2, smin(s2:-2, s2:0));
    assert_eq(s2:-2, smin(s2:-2, s2:1));

    assert_eq(s2:-2, smin(s2:-1, s2:-2));
    assert_eq(s2:-1, smin(s2:-1, s2:-1));
    assert_eq(s2:-1, smin(s2:-1, s2:0));
    assert_eq(s2:-1, smin(s2:-1, s2:1));
}

// Returns the minimum of two unsigned integers.
pub fn umin<N: u32>(x: uN[N], y: uN[N]) -> uN[N] { if x < y { x } else { y } }

#[test]
fn umin_test() {
    assert_eq(u1:0, umin(u1:1, u1:0));
    assert_eq(u1:1, umin(u1:1, u1:1));
    assert_eq(u2:2, umin(u2:3, u2:2));
}

// Returns unsigned add of x (N bits) and y (M bits) as a max(N,M)+1 bit value.
pub fn uadd<N: u32, M: u32, R: u32 = {umax(N, M) + u32:1}>(x: uN[N], y: uN[M]) -> uN[R] {
    (x as uN[R]) + (y as uN[R])
}

// Returns signed add of x (N bits) and y (M bits) as a max(N,M)+1 bit value.
pub fn sadd<N: u32, M: u32, R: u32 = {umax(N, M) + u32:1}>(x: sN[N], y: sN[M]) -> sN[R] {
    (x as sN[R]) + (y as sN[R])
}

#[test]
fn uadd_test() {
    assert_eq(u4:4, uadd(u3:2, u3:2));
    assert_eq(u4:0b0100, uadd(u3:0b011, u3:0b001));
    assert_eq(u4:0b1110, uadd(u3:0b111, u3:0b111));
}

#[test]
fn sadd_test() {
    assert_eq(s4:4, sadd(s3:2, s3:2));
    assert_eq(s4:0, sadd(s3:1, s3:-1));
    assert_eq(s4:0b0100, sadd(s3:0b011, s2:0b01));
    assert_eq(s4:-8, sadd(s3:-4, s3:-4));
}

// Returns unsigned mul of x (N bits) and y (M bits) as an N+M bit value.
pub fn umul<N: u32, M: u32, R: u32 = {N + M}>(x: uN[N], y: uN[M]) -> uN[R] {
    (x as uN[R]) * (y as uN[R])
}

// Returns signed mul of x (N bits) and y (M bits) as an N+M bit value.
pub fn smul<N: u32, M: u32, R: u32 = {N + M}>(x: sN[N], y: sN[M]) -> sN[R] {
    (x as sN[R]) * (y as sN[R])
}

#[test]
fn umul_test() {
    assert_eq(u6:4, umul(u3:2, u3:2));
    assert_eq(u4:0b0011, umul(u2:0b11, u2:0b01));
    assert_eq(u4:0b1001, umul(u2:0b11, u2:0b11));
}

#[test]
fn smul_test() {
    assert_eq(s6:4, smul(s3:2, s3:2));
    assert_eq(s4:0b1111, smul(s2:0b11, s2:0b01));
}

// Calculate x / y one bit at a time. This is an alternative to using
// the division operator '/' which may not synthesize nicely.
pub fn iterative_div<N: u32, DN: u32 = {N * u32:2}>(x: uN[N], y: uN[N]) -> uN[N] {
    let init_shift_amount = ((N as uN[N]) - uN[N]:1);
    let x = x as uN[DN];

    let (_, _, _, div_result) = for
        (idx, (shifted_y, shifted_index_bit, running_product, running_result)) in range(u32:0, N) {
        // Increment running_result by current power of 2
        // if the prodcut running_result * y < x.
        let inc_running_result = running_result | shifted_index_bit;
        let inc_running_product = running_product + shifted_y;
        let (running_result, running_product) = if inc_running_product <= x {
            (inc_running_result, inc_running_product)
        } else {
            (running_result, running_product)
        };

        // Shift to next (lower) power of 2.
        let shifted_y = shifted_y >> uN[N]:1;
        let shifted_index_bit = shifted_index_bit >> uN[N]:1;

        (shifted_y, shifted_index_bit, running_product, running_result)
    }((
        (y as uN[DN]) << (init_shift_amount as uN[DN]), uN[N]:1 << init_shift_amount, uN[DN]:0,
        uN[N]:0,
    ));

    div_result
}

#[test]
fn iterative_div_test() {
    // Power of 2.
    assert_eq(u4:0, iterative_div(u4:8, u4:15));
    assert_eq(u4:1, iterative_div(u4:8, u4:8));
    assert_eq(u4:2, iterative_div(u4:8, u4:4));
    assert_eq(u4:4, iterative_div(u4:8, u4:2));
    assert_eq(u4:8, iterative_div(u4:8, u4:1));
    assert_eq(u4:8 / u4:0, iterative_div(u4:8, u4:0));
    assert_eq(u4:15, iterative_div(u4:8, u4:0));

    // Non-powers-of-2.
    assert_eq(u32:6, iterative_div(u32:18, u32:3));
    assert_eq(u32:6, iterative_div(u32:36, u32:6));
    assert_eq(u32:6, iterative_div(u32:48, u32:8));
    assert_eq(u32:20, iterative_div(u32:900, u32:45));

    // Results w/ remainder.
    assert_eq(u32:6, iterative_div(u32:20, u32:3));
    assert_eq(u32:6, iterative_div(u32:41, u32:6));
    assert_eq(u32:6, iterative_div(u32:55, u32:8));
    assert_eq(u32:20, iterative_div(u32:944, u32:45));
}

// Returns the value of x-1 with saturation at 0.
pub fn bounded_minus_1<N: u32>(x: uN[N]) -> uN[N] { if x == uN[N]:0 { x } else { x - uN[N]:1 } }

// Extracts the LSb (least significant bit) from the value `x` and returns it.
pub fn lsb<N: u32>(x: uN[N]) -> u1 { x as u1 }

#[test]
fn lsb_test() {
    assert_eq(u1:0, lsb(u2:0b00));
    assert_eq(u1:1, lsb(u2:0b01));
    assert_eq(u1:1, lsb(u2:0b11));
    assert_eq(u1:0, lsb(u2:0b10));
}

// Returns the absolute value of x as a signed number.
pub fn abs<BITS: u32>(x: sN[BITS]) -> sN[BITS] { if x < sN[BITS]:0 { -x } else { x } }

// Converts an array of N bools to a bits[N] value.
//
// The bool at index 0 in the array because the MSb (most significant bit) in
// the result.
pub fn convert_to_bits_msb0<N: u32>(x: bool[N]) -> uN[N] {
    for (i, accum): (u32, uN[N]) in range(u32:0, N) {
        accum | (x[i] as uN[N]) << ((N - i - u32:1) as uN[N])
    }(uN[N]:0)
}

#[test]
fn convert_to_bits_msb0_test() {
    assert_eq(u3:0b000, convert_to_bits_msb0(bool[3]:[false, false, false]));
    assert_eq(u3:0b001, convert_to_bits_msb0(bool[3]:[false, false, true]));
    assert_eq(u3:0b010, convert_to_bits_msb0(bool[3]:[false, true, false]));
    assert_eq(u3:0b011, convert_to_bits_msb0(bool[3]:[false, true, true]));
    assert_eq(u3:0b100, convert_to_bits_msb0(bool[3]:[true, false, false]));
    assert_eq(u3:0b110, convert_to_bits_msb0(bool[3]:[true, true, false]));
    assert_eq(u3:0b111, convert_to_bits_msb0(bool[3]:[true, true, true]));
}

// Converts a bits[N] values to an array of N bools.
//
// This variant puts the LSb (least significant bit) of the word at index 0 in
// the resulting array.
pub fn convert_to_bools_lsb0<N: u32>(x: uN[N]) -> bool[N] {
    for (idx, partial): (u32, bool[N]) in range(u32:0, N) {
        update(partial, idx, x[idx+:bool])
    }(bool[N]:[false, ...])
}

#[test]
fn convert_to_bools_lsb0_test() {
    assert_eq(convert_to_bools_lsb0(u1:1), bool[1]:[true]);
    assert_eq(convert_to_bools_lsb0(u2:0b01), bool[2]:[true, false]);
    assert_eq(convert_to_bools_lsb0(u2:0b10), bool[2]:[false, true]);
    assert_eq(convert_to_bools_lsb0(u3:0b000), bool[3]:[false, false, false]);
    assert_eq(convert_to_bools_lsb0(u3:0b001), bool[3]:[true, false, false]);
    assert_eq(convert_to_bools_lsb0(u3:0b010), bool[3]:[false, true, false]);
    assert_eq(convert_to_bools_lsb0(u3:0b011), bool[3]:[true, true, false]);
    assert_eq(convert_to_bools_lsb0(u3:0b100), bool[3]:[false, false, true]);
    assert_eq(convert_to_bools_lsb0(u3:0b110), bool[3]:[false, true, true]);
    assert_eq(convert_to_bools_lsb0(u3:0b111), bool[3]:[true, true, true]);
}

#[quickcheck]
fn convert_to_from_bools(x: u4) -> bool {
    convert_to_bits_msb0(array_rev(convert_to_bools_lsb0(x))) == x
}

// Returns (found, index) given array and the element to find within the array.
//
// Note that when found is false, the index is 0 -- 0 is provided instead of a
// value like -1 to prevent out-of-bounds accesses from occurring if the index
// is used in a match expression (which will eagerly evaluate all of its arms),
// to prevent it from creating an error at simulation time if the value is
// ultimately discarded from the unselected match arm.
pub fn find_index<BITS: u32, ELEMS: u32>(array: uN[BITS][ELEMS], x: uN[BITS]) -> (bool, u32) {
    // Compute all the positions that are equal to our target.
    let bools: bool[ELEMS] = for (i, accum): (u32, bool[ELEMS]) in range(u32:0, ELEMS) {
        update(accum, i, array[i] == x)
    }((bool[ELEMS]:[false, ...]));

    let x: uN[ELEMS] = convert_to_bits_msb0(bools);
    let index = clz(x);
    let found: bool = or_reduce(x);
    (found, if found { index as u32 } else { u32:0 })
}

#[test]
fn find_index_test() {
    let haystack = u3[4]:[0b001, 0b010, 0b100, 0b111];
    assert_eq((true, u32:1), find_index(haystack, u3:0b010));
    assert_eq((true, u32:3), find_index(haystack, u3:0b111));
    assert_eq((false, u32:0), find_index(haystack, u3:0b000));
}

// Concatenates 3 values of arbitrary bitwidths to a single value.
pub fn concat3<X: u32, Y: u32, Z: u32, R: u32 = {X + Y + Z}>
    (x: bits[X], y: bits[Y], z: bits[Z]) -> bits[R] {
    x ++ y ++ z
}

#[test]
fn concat3_test() { assert_eq(u12:0b111000110010, concat3(u6:0b111000, u4:0b1100, u2:0b10)); }

// Returns the ceiling of (x divided by y).
pub fn ceil_div<N: u32>(x: uN[N], y: uN[N]) -> uN[N] {
    let usual = (x - uN[N]:1) / y + uN[N]:1;
    if x > uN[N]:0 { usual } else { uN[N]:0 }
}

#[test]
fn ceil_div_test() {
    assert_eq(ceil_div(u32:6, u32:2), u32:3);
    assert_eq(ceil_div(u32:5, u32:2), u32:3);
    assert_eq(ceil_div(u32:4, u32:2), u32:2);
    assert_eq(ceil_div(u32:3, u32:2), u32:2);
    assert_eq(ceil_div(u32:2, u32:2), u32:1);
    assert_eq(ceil_div(u32:1, u32:2), u32:1);
    assert_eq(ceil_div(u32:0, u32:2), u32:0);

    assert_eq(ceil_div(u8:6, u8:3), u8:2);
    assert_eq(ceil_div(u8:5, u8:3), u8:2);
    assert_eq(ceil_div(u8:4, u8:3), u8:2);
    assert_eq(ceil_div(u8:3, u8:3), u8:1);
    assert_eq(ceil_div(u8:2, u8:3), u8:1);
    assert_eq(ceil_div(u8:1, u8:3), u8:1);
    assert_eq(ceil_div(u8:0, u8:3), u8:0);
}

// Returns `x` rounded up to the nearest multiple of `y`.
pub fn round_up_to_nearest(x: u32, y: u32) -> u32 { (ceil_div(x, y) * y) as u32 }

#[test]
fn round_up_to_nearest_test() {
    assert_eq(u32:4, round_up_to_nearest(u32:3, u32:2));
    assert_eq(u32:4, round_up_to_nearest(u32:4, u32:2));
}

// Rotate `x` right by `y` bits.
pub fn rrot<N: u32>(x: bits[N], y: bits[N]) -> bits[N] { (x >> y) | (x << ((N as bits[N]) - y)) }

#[test]
fn rrot_test() {
    assert_eq(bits[3]:0b101, rrot(bits[3]:0b011, bits[3]:1));
    assert_eq(bits[3]:0b011, rrot(bits[3]:0b110, bits[3]:1));
}

// Returns `floor(log2(x))`, with one exception:
//
// When x=0, this function differs from the true mathematical function:
// flog2(0) = 0
// floor(log2(0)) = -infinity
//
// This function is frequently used to calculate the number of bits required to
// represent an unsigned integer `n` to define flog2(0) = 0.
//
// Example: flog2(7) = 2, flog2(8) = 3.
pub fn flog2<N: u32>(x: bits[N]) -> bits[N] {
    if x >= bits[N]:1 { (N as bits[N]) - clz(x) - bits[N]:1 } else { bits[N]:0 }
}

#[test]
fn flog2_test() {
    assert_eq(u32:0, flog2(u32:0));
    assert_eq(u32:0, flog2(u32:1));
    assert_eq(u32:1, flog2(u32:2));
    assert_eq(u32:1, flog2(u32:3));
    assert_eq(u32:2, flog2(u32:4));
    assert_eq(u32:2, flog2(u32:5));
    assert_eq(u32:2, flog2(u32:6));
    assert_eq(u32:2, flog2(u32:7));
    assert_eq(u32:3, flog2(u32:8));
    assert_eq(u32:3, flog2(u32:9));
}

// Returns `ceiling(log2(x))`, with one exception:
//
// When x=0, this function differs from the true mathematical function:
// clog2(0) = 0
// ceiling(log2(0)) = -infinity
//
// This function is frequently used to calculate the number of bits required to
// represent `x` possibilities. With this interpretation, it is sensible
// to define clog2(0) = 0.
//
// Example: clog2(7) = 3
pub fn clog2<N: u32>(x: bits[N]) -> bits[N] {
    if x >= bits[N]:1 { (N as bits[N]) - clz(x - bits[N]:1) } else { bits[N]:0 }
}

#[test]
fn clog2_test() {
    assert_eq(u32:0, clog2(u32:0));
    assert_eq(u32:0, clog2(u32:1));
    assert_eq(u32:1, clog2(u32:2));
    assert_eq(u32:2, clog2(u32:3));
    assert_eq(u32:2, clog2(u32:4));
    assert_eq(u32:3, clog2(u32:5));
    assert_eq(u32:3, clog2(u32:6));
    assert_eq(u32:3, clog2(u32:7));
    assert_eq(u32:3, clog2(u32:8));
    assert_eq(u32:4, clog2(u32:9));
}

// Returns true when x is a non-zero power-of-two.
pub fn is_pow2<N: u32>(x: uN[N]) -> bool { x > uN[N]:0 && (x & (x - uN[N]:1) == uN[N]:0) }

#[test]
fn is_pow2_test() {
    assert_eq(is_pow2(u32:0), false);
    assert_eq(is_pow2(u32:1), true);
    assert_eq(is_pow2(u32:2), true);
    assert_eq(is_pow2(u32:3), false);
    assert_eq(is_pow2(u32:4), true);
    assert_eq(is_pow2(u32:5), false);
    assert_eq(is_pow2(u32:6), false);
    assert_eq(is_pow2(u32:7), false);
    assert_eq(is_pow2(u32:8), true);

    // Test parametric bitwidth.
    assert_eq(is_pow2(u8:0), false);
    assert_eq(is_pow2(u8:1), true);
    assert_eq(is_pow2(u8:2), true);
    assert_eq(is_pow2(u8:3), false);
    assert_eq(is_pow2(u8:4), true);
    assert_eq(is_pow2(u8:5), false);
    assert_eq(is_pow2(u8:6), false);
    assert_eq(is_pow2(u8:7), false);
    assert_eq(is_pow2(u8:8), true);
}

// Returns x % y where y must be a non-zero power-of-two.
pub fn mod_pow2<N: u32>(x: bits[N], y: bits[N]) -> bits[N] {
    // TODO(leary): 2020-06-11 Add assertion y is a power of two and non-zero.
    x & (y - bits[N]:1)
}

#[test]
fn mod_pow2_test() {
    assert_eq(u32:1, mod_pow2(u32:5, u32:4));
    assert_eq(u32:0, mod_pow2(u32:4, u32:4));
    assert_eq(u32:3, mod_pow2(u32:3, u32:4));
}

// Returns x / y where y must be a non-zero power-of-two.
pub fn div_pow2<N: u32>(x: bits[N], y: bits[N]) -> bits[N] {
    // TODO(leary): 2020-06-11 Add assertion y is a power of two and non-zero.
    x >> clog2(y)
}

#[test]
fn div_pow2_test() {
    assert_eq(u32:1, div_pow2(u32:5, u32:4));
    assert_eq(u32:1, div_pow2(u32:4, u32:4));
    assert_eq(u32:0, div_pow2(u32:3, u32:4));
}

// Returns a value with X bits set (of type bits[X]).
pub fn mask_bits<X: u32>() -> bits[X] { !bits[X]:0 }

#[test]
fn mask_bits_test() {
    assert_eq(u8:0xff, mask_bits<u32:8>());
    assert_eq(u13:0x1fff, mask_bits<u32:13>());
}

// "Explicit signed comparison" helpers for working with unsigned values, can be
// a bit more convenient and a bit more explicit intent than doing casting of
// left hand side and right hand side.

pub fn sge<N: u32>(x: uN[N], y: uN[N]) -> bool { (x as sN[N]) >= (y as sN[N]) }

pub fn sgt<N: u32>(x: uN[N], y: uN[N]) -> bool { (x as sN[N]) > (y as sN[N]) }

pub fn sle<N: u32>(x: uN[N], y: uN[N]) -> bool { (x as sN[N]) <= (y as sN[N]) }

pub fn slt<N: u32>(x: uN[N], y: uN[N]) -> bool { (x as sN[N]) < (y as sN[N]) }

#[test]
fn test_scmps() {
    assert_eq(sge(u2:3, u2:1), false);
    assert_eq(sgt(u2:3, u2:1), false);
    assert_eq(sle(u2:3, u2:1), true);
    assert_eq(slt(u2:3, u2:1), true);
}

// Performs integer exponentiation as in Hacker's Delight, section 11-3.
// Only nonnegative exponents are allowed, hence the uN parameter for spow.
pub fn upow<N: u32>(x: uN[N], n: uN[N]) -> uN[N] {
    let result = uN[N]:1;
    let p = x;

    let work = for (i, (n, p, result)) in range(u32:0, N) {
        let result = if (n & uN[N]:1) == uN[N]:1 { result * p } else { result };

        (n >> 1, p * p, result)
    }((n, p, result));
    work.2
}

pub fn spow<N: u32>(x: sN[N], n: uN[N]) -> sN[N] {
    let result = sN[N]:1;
    let p = x;

    let work = for (i, (n, p, result)): (u32, (uN[N], sN[N], sN[N])) in range(u32:0, N) {
        let result = if (n & uN[N]:1) == uN[N]:1 { result * p } else { result };

        (n >> uN[N]:1, p * p, result)
    }((n, p, result));
    work.2
}

#[test]
fn test_upow() {
    assert_eq(upow(u32:2, u32:2), u32:4);
    assert_eq(upow(u32:2, u32:20), u32:0x100000);
    assert_eq(upow(u32:3, u32:20), u32:0xcfd41b91);
    assert_eq(upow(u32:1, u32:20), u32:0x1);
    assert_eq(upow(u32:1, u32:20), u32:0x1);
}

#[test]
fn test_spow() {
    assert_eq(spow(s32:2, u32:2), s32:4);
    assert_eq(spow(s32:2, u32:20), s32:0x100000);
    assert_eq(spow(s32:3, u32:20), s32:0xcfd41b91);
    assert_eq(spow(s32:1, u32:20), s32:0x1);
    assert_eq(spow(s32:1, u32:20), s32:0x1);
}

// Count the number of bits that are 1.
pub fn popcount<N: u32>(x: bits[N]) -> bits[N] {
    let (x, acc) = for (i, (x, acc)): (u32, (bits[N], bits[N])) in range(u32:0, N) {
        let acc = if (x & bits[N]:1) as u1 { acc + bits[N]:1 } else { acc };
        let x = x >> 1;
        (x, acc)
    }((x, bits[N]:0));
    (acc)
}

#[test]
fn test_popcount() {
    assert_eq(popcount(u17:0xa5a5), u17:8);
    assert_eq(popcount(u17:0x1a5a5), u17:9);
    assert_eq(popcount(u1:0x0), u1:0);
    assert_eq(popcount(u1:0x1), u1:1);
    assert_eq(popcount(u32:0xffffffff), u32:32);
}

// Converts an unsigned number to a signed number of the same width.
//
// This is the moral equivalent of:
//
//     x as sN[std::sizeof_unsigned(x)]
//
// That is, you might use it when you don't want to figure out the width of x
// in order to perform a cast, you just know that the unsigned number you have
// you want to turn signed.
pub fn to_signed<N: u32>(x: uN[N]) -> sN[N] { x as sN[N] }

#[test]
fn test_to_signed() {
    let x = u32:42;
    assert_eq(s32:42, to_signed(x));
    assert_eq(x as sN[sizeof_unsigned(x)], to_signed(x));
    let x = u8:42;
    assert_eq(s8:42, to_signed(x));
    assert_eq(x as sN[sizeof_unsigned(x)], to_signed(x));
}

// As with to_signed but for signed-to-unsigned conversion.
pub fn to_unsigned<N: u32>(x: sN[N]) -> uN[N] { x as uN[N] }

#[test]
fn test_to_unsigned() {
    let x = s32:42;
    assert_eq(u32:42, to_unsigned(x));
    assert_eq(x as uN[sizeof_signed(x)], to_unsigned(x));
    let x = s8:42;
    assert_eq(u8:42, to_unsigned(x));
    assert_eq(x as uN[sizeof_signed(x)], to_unsigned(x));
}

// Adds two unsigned integers and detects for overflow.
//
// uadd_with_overflow<V: u32>(x: uN[N], y : uN[M]) returns a 2-tuple
// indicating overflow (boolean) and a sum (x+y) as uN[V).  An overflow
// occurs if the result does not fit within a uN[V].
//
// Example usage:
//  let result : (bool, u16) = uadd_with_overflow<u32:16>(x, y);
//
pub fn uadd_with_overflow
    <V: u32, N: u32, M: u32, MAX_N_M: u32 = {umax(N, M)}, MAX_N_M_V: u32 = {umax(MAX_N_M, V)}>
    (x: uN[N], y: uN[M]) -> (bool, uN[V]) {

    let x_extended = widening_cast<uN[MAX_N_M_V + u32:1]>(x);
    let y_extended = widening_cast<uN[MAX_N_M_V + u32:1]>(y);

    let full_result: uN[MAX_N_M_V + u32:1] = x_extended + y_extended;
    let narrowed_result = full_result as uN[V];
    let overflow_detected = or_reduce(full_result[V as s32:]);

    (overflow_detected, narrowed_result)
}

#[test]
fn test_uadd_with_overflow() {
    assert_eq(uadd_with_overflow<u32:1>(u4:0, u5:0), (false, u1:0));
    assert_eq(uadd_with_overflow<u32:1>(u4:1, u5:0), (false, u1:1));
    assert_eq(uadd_with_overflow<u32:1>(u4:1, u5:1), (true, u1:0));
    assert_eq(uadd_with_overflow<u32:1>(u4:2, u5:1), (true, u1:1));

    assert_eq(uadd_with_overflow<u32:4>(u4:15, u3:0), (false, u4:15));
    assert_eq(uadd_with_overflow<u32:4>(u4:8, u3:7), (false, u4:15));
    assert_eq(uadd_with_overflow<u32:4>(u4:9, u3:7), (true, u4:0));
    assert_eq(uadd_with_overflow<u32:4>(u4:10, u3:6), (true, u4:0));
    assert_eq(uadd_with_overflow<u32:4>(u4:11, u3:6), (true, u4:1));
}

// Extract bits given a fixed-point integer with a constant offset.
//   i.e. let x_extended = x as uN[max(unsigned_sizeof(x) + fixed_shift, to_exclusive)];
//        (x_extended << fixed_shift)[from_inclusive:to_exclusive]
//
// This function behaves as-if x has reasonably infinite precision so that
// the result is zero-padded if from_inclusive or to_exclusive are out of
// range of the original x's bitwidth.
//
// If to_exclusive <= from_exclusive, the result will be a zero-bit uN[0].
pub fn extract_bits
    <from_inclusive: u32, to_exclusive: u32, fixed_shift: u32, N: u32,
     extract_width: u32 = {smax(s32:0, to_exclusive as s32 - from_inclusive as s32) as u32}>
    (x: uN[N]) -> uN[extract_width] {
    if to_exclusive <= from_inclusive {
        uN[extract_width]:0
    } else {
        // With a non-zero fixed width, all lower bits of index < fixed_shift are
        // are zero.
        let lower_bits =
            uN[checked_cast<u32>(smax(s32:0, fixed_shift as s32 - from_inclusive as s32))]:0;

        // Based on the input of N bits and a fixed shift, there are an effective
        // count of N + fixed_shift known bits.  All bits of index >
        // N + fixed_shift - 1 are zero's.
        const UPPER_BIT_COUNT = checked_cast<u32>(
            smax(s32:0, N as s32 + fixed_shift as s32 - to_exclusive as s32 - s32:1));
        let upper_bits = uN[UPPER_BIT_COUNT]:0;

        if fixed_shift < from_inclusive {
            // The bits extracted start within or after the middle span.
            //  upper_bits ++ middle_bits
            let middle_bits = upper_bits ++
                              x[smin(from_inclusive as s32 - fixed_shift as s32, N as s32)
                                :smin(to_exclusive as s32 - fixed_shift as s32, N as s32)];
            (upper_bits ++ middle_bits) as uN[extract_width]
        } else if fixed_shift <= to_exclusive {
            // The bits extracted start within the fixed_shift span.
            let middle_bits = x[0:smin(to_exclusive as s32 - fixed_shift as s32, N as s32)];

            (upper_bits ++ middle_bits ++ lower_bits) as uN[extract_width]
        } else {
            uN[extract_width]:0
        }
    }
}

#[test]
fn test_extract_bits() {
    assert_eq(extract_bits<u32:4, u32:4, u32:0>(u4:0x9), uN[0]:0);
    assert_eq(extract_bits<u32:0, u32:4, u32:0>(u4:0x9), u4:0x9);  // 0b[1001]

    assert_eq(extract_bits<u32:0, u32:5, u32:0>(u4:0xf), u5:0xf);  // 0b[01111]
    assert_eq(extract_bits<u32:0, u32:5, u32:1>(u4:0xf), u5:0x1e);  // 0b0[11110]
    assert_eq(extract_bits<u32:0, u32:5, u32:2>(u4:0xf), u5:0x1c);  // 0b1[11100]
    assert_eq(extract_bits<u32:0, u32:5, u32:3>(u4:0xf), u5:0x18);  // 0b11[11000]
    assert_eq(extract_bits<u32:0, u32:5, u32:4>(u4:0xf), u5:0x10);  // 0b111[10000]
    assert_eq(extract_bits<u32:0, u32:5, u32:5>(u4:0xf), u5:0x0);  // 0b1111[00000]

    assert_eq(extract_bits<u32:2, u32:5, u32:0>(u4:0xf), u3:0x3);  // 0b[011]11
    assert_eq(extract_bits<u32:2, u32:5, u32:1>(u4:0xf), u3:0x7);  // 0b[111]10
    assert_eq(extract_bits<u32:2, u32:5, u32:2>(u4:0xf), u3:0x7);  // 0b1[111]00
    assert_eq(extract_bits<u32:2, u32:5, u32:3>(u4:0xf), u3:0x6);  // 0b11[110]00
    assert_eq(extract_bits<u32:2, u32:5, u32:4>(u4:0xf), u3:0x4);  // 0b111[100]00
    assert_eq(extract_bits<u32:2, u32:5, u32:5>(u4:0xf), u3:0x0);  // 0b1111[000]00

    assert_eq(extract_bits<u32:0, u32:4, u32:0>(u4:0xf), u4:0xf);  // 0b[1111]
    assert_eq(extract_bits<u32:0, u32:4, u32:1>(u4:0xf), u4:0xe);  // 0b1[1110]
    assert_eq(extract_bits<u32:0, u32:4, u32:2>(u4:0xf), u4:0xc);  // 0b11[1100]
    assert_eq(extract_bits<u32:0, u32:4, u32:3>(u4:0xf), u4:0x8);  // 0b111[1000]
    assert_eq(extract_bits<u32:0, u32:4, u32:4>(u4:0xf), u4:0x0);  // 0b1111[0000]
    assert_eq(extract_bits<u32:0, u32:4, u32:5>(u4:0xf), u4:0x0);  // 0b11110[0000]

    assert_eq(extract_bits<u32:1, u32:4, u32:0>(u4:0xf), u3:0x7);  // 0b[111]1
    assert_eq(extract_bits<u32:1, u32:4, u32:1>(u4:0xf), u3:0x7);  // 0b1[111]0
    assert_eq(extract_bits<u32:1, u32:4, u32:2>(u4:0xf), u3:0x6);  // 0b11[110]0
    assert_eq(extract_bits<u32:1, u32:4, u32:3>(u4:0xf), u3:0x4);  // 0b111[100]0
    assert_eq(extract_bits<u32:1, u32:4, u32:4>(u4:0xf), u3:0x0);  // 0b1111[000]0
    assert_eq(extract_bits<u32:1, u32:4, u32:5>(u4:0xf), u3:0x0);  // 0b11110[000]0

    assert_eq(extract_bits<u32:2, u32:4, u32:0>(u4:0xf), u2:0x3);  // 0b[11]11
    assert_eq(extract_bits<u32:2, u32:4, u32:1>(u4:0xf), u2:0x3);  // 0b1[11]10
    assert_eq(extract_bits<u32:2, u32:4, u32:2>(u4:0xf), u2:0x3);  // 0b11[11]00
    assert_eq(extract_bits<u32:2, u32:4, u32:3>(u4:0xf), u2:0x2);  // 0b111[10]00
    assert_eq(extract_bits<u32:2, u32:4, u32:4>(u4:0xf), u2:0x0);  // 0b1111[00]00
    assert_eq(extract_bits<u32:2, u32:4, u32:5>(u4:0xf), u2:0x0);  // 0b11110[00]00

    assert_eq(extract_bits<u32:3, u32:4, u32:0>(u4:0xf), u1:0x1);  // 0b[1]111
    assert_eq(extract_bits<u32:3, u32:4, u32:1>(u4:0xf), u1:0x1);  // 0b1[1]110
    assert_eq(extract_bits<u32:3, u32:4, u32:2>(u4:0xf), u1:0x1);  // 0b11[1]100
    assert_eq(extract_bits<u32:3, u32:4, u32:3>(u4:0xf), u1:0x1);  // 0b111[1]000
    assert_eq(extract_bits<u32:3, u32:4, u32:4>(u4:0xf), u1:0x0);  // 0b1111[0]000
    assert_eq(extract_bits<u32:3, u32:4, u32:5>(u4:0xf), u1:0x0);  // 0b11110[0]000

    assert_eq(extract_bits<u32:0, u32:3, u32:0>(u4:0xf), u3:0x7);  // 0b1[111]
    assert_eq(extract_bits<u32:0, u32:3, u32:1>(u4:0xf), u3:0x6);  // 0b11[110]
    assert_eq(extract_bits<u32:0, u32:3, u32:2>(u4:0xf), u3:0x4);  // 0b111[100]
    assert_eq(extract_bits<u32:0, u32:3, u32:3>(u4:0xf), u3:0x0);  // 0b1111[000]
    assert_eq(extract_bits<u32:0, u32:3, u32:4>(u4:0xf), u3:0x0);  // 0b11110[000]
    assert_eq(extract_bits<u32:0, u32:3, u32:5>(u4:0xf), u3:0x0);  // 0b111100[000]

    assert_eq(extract_bits<u32:1, u32:3, u32:0>(u4:0xf), u2:0x3);  // 0b1[11]1
    assert_eq(extract_bits<u32:1, u32:3, u32:1>(u4:0xf), u2:0x3);  // 0b11[11]0
    assert_eq(extract_bits<u32:1, u32:3, u32:2>(u4:0xf), u2:0x2);  // 0b111[10]0
    assert_eq(extract_bits<u32:1, u32:3, u32:3>(u4:0xf), u2:0x0);  // 0b1111[00]0
    assert_eq(extract_bits<u32:1, u32:3, u32:4>(u4:0xf), u2:0x0);  // 0b11110[00]0
    assert_eq(extract_bits<u32:1, u32:3, u32:5>(u4:0xf), u2:0x0);  // 0b111100[00]0

    assert_eq(extract_bits<u32:2, u32:3, u32:0>(u4:0xf), u1:0x1);  // 0b1[1]11
    assert_eq(extract_bits<u32:2, u32:3, u32:1>(u4:0xf), u1:0x1);  // 0b11[1]10
    assert_eq(extract_bits<u32:2, u32:3, u32:2>(u4:0xf), u1:0x1);  // 0b111[1]00
    assert_eq(extract_bits<u32:2, u32:3, u32:3>(u4:0xf), u1:0x0);  // 0b1111[0]00
    assert_eq(extract_bits<u32:2, u32:3, u32:4>(u4:0xf), u1:0x0);  // 0b11110[0]00
    assert_eq(extract_bits<u32:2, u32:3, u32:5>(u4:0xf), u1:0x0);  // 0b111100[0]00
}

// Multiplies two numbers and detects for overflow.
//
// umul_with_overflow<V: u32>(x: uN[N], y : uN[M]) returns a 2-tuple
// indicating overflow (boolean) and a product (x*y) as uN[V].  An overflow
// occurs if the result does not fit within a uN[V].
//
// Example usage:
//  let result : (bool, u16) = umul_with_overflow<u32:16>(x, y);
//
pub fn umul_with_overflow
    <V: u32, N: u32, M: u32, N_lower_bits: u32 = {N >> u32:1},
     N_upper_bits: u32 = {N - N_lower_bits}, M_lower_bits: u32 = {M >> u32:1},
     M_upper_bits: u32 = {M - M_lower_bits},
     Min_N_M_lower_bits: u32 = {umin(N_lower_bits, M_lower_bits)}, N_Plus_M: u32 = {N + M}>
    (x: uN[N], y: uN[M]) -> (bool, uN[V]) {
    // Break x and y into two halves.
    // x = x1 ++ x0,
    // y = y1 ++ x1,
    let x1 = x[N_lower_bits as s32:];
    let x0 = x[s32:0:N_lower_bits as s32];

    let y1 = y[M_lower_bits as s32:];
    let y0 = y[s32:0:M_lower_bits as s32];

    // Bits [0 : N_lower_bits+M_lower_bits]]
    let x0y0: uN[N_lower_bits + M_lower_bits] = umul(x0, y0);

    // Bits [M_lower_bits +: N_upper_bits+M_lower_bits]
    let x1y0: uN[N_upper_bits + M_lower_bits] = umul(x1, y0);

    // Bits [N_lower_bits +: N_lower_bits+M_upper_bits]
    let x0y1: uN[M_upper_bits + N_lower_bits] = umul(x0, y1);

    // Bits [N_lower_bits+M_lower_bits += N_upper_bits+M_upper_bits]
    let x1y1: uN[N_upper_bits + M_upper_bits] = umul(x1, y1);

    // Break the above numbers into three buckets
    //  [0: min(N_lower_bits, M_lower_bits)] --> only from x0y0
    //  [min(N_lower_bits, M_lower_bits : V] --> need to add together
    //  [V : N+M] --> need to or_reduce
    let x0y0_a = extract_bits<u32:0, Min_N_M_lower_bits, u32:0>(x0y0);
    let x0y0_b = extract_bits<Min_N_M_lower_bits, V, u32:0>(x0y0);
    let x0y0_c = extract_bits<V, N_Plus_M, u32:0>(x0y0);

    // x1 has a shift of N_lower_bits
    let x1y0_b = extract_bits<Min_N_M_lower_bits, V, N_lower_bits>(x1y0);
    let x1y0_c = extract_bits<V, N_Plus_M, N_lower_bits>(x1y0);

    // y1 has a shift of M_lower_bits
    let x0y1_b = extract_bits<Min_N_M_lower_bits, V, M_lower_bits>(x0y1);
    let x0y1_c = extract_bits<V, N_Plus_M, M_lower_bits>(x0y1);

    // x1y1 has a shift of N_lower_bits + M_lower_bits
    let x1y1_b = extract_bits<Min_N_M_lower_bits, V, {N_lower_bits + M_lower_bits}>(x1y1);
    let x1y1_c = extract_bits<V, N_Plus_M, {N_lower_bits + M_lower_bits}>(x1y1);

    // Add partial shifts to obtain the narrowed results, keeping 2 bits for overflow.
    // (x0y0_b + x1y0_b + x1y1_b + x1y1_b) ++ x0y0a
    let x0y0_b_extended = widening_cast<uN[V - Min_N_M_lower_bits + u32:2]>(x0y0_b);
    let x0y1_b_extended = widening_cast<uN[V - Min_N_M_lower_bits + u32:2]>(x0y1_b);
    let x1y0_b_extended = widening_cast<uN[V - Min_N_M_lower_bits + u32:2]>(x1y0_b);
    let x1y1_b_extended = widening_cast<uN[V - Min_N_M_lower_bits + u32:2]>(x1y1_b);
    let narrowed_result_upper =
        x0y0_b_extended + x1y0_b_extended + x0y1_b_extended + x1y1_b_extended;
    let overflow_narrowed_result_upper_sum =
        or_reduce(narrowed_result_upper[V as s32 - Min_N_M_lower_bits as s32:]);

    let partial_narrowed_result =
        narrowed_result_upper[0:V as s32 - Min_N_M_lower_bits as s32] ++ x0y0_a;

    let narrowed_result = partial_narrowed_result[0:V as s32];
    let overflow_detected = or_reduce(x0y0_c) || or_reduce(x0y1_c) || or_reduce(x1y0_c) ||
                            or_reduce(x1y1_c) || or_reduce(partial_narrowed_result[V as s32:]) ||
                            overflow_narrowed_result_upper_sum;

    (overflow_detected, narrowed_result)
}

// TODO(tedhong): 2023-08-11 Exhaustively test umul_with_overflow for
// certain bitwith combinations.
#[test]
fn test_umul_with_overflow() {
    assert_eq(umul_with_overflow<u32:1>(u4:0, u4:0), (false, u1:0));
    assert_eq(umul_with_overflow<u32:1>(u4:15, u4:0), (false, u1:0));
    assert_eq(umul_with_overflow<u32:1>(u4:1, u4:1), (false, u1:1));
    assert_eq(umul_with_overflow<u32:1>(u4:2, u4:1), (true, u1:0));
    assert_eq(umul_with_overflow<u32:1>(u4:8, u4:8), (true, u1:0));
    assert_eq(umul_with_overflow<u32:1>(u4:15, u4:15), (true, u1:1));

    assert_eq(umul_with_overflow<u32:4>(u4:0, u3:0), (false, u4:0));
    assert_eq(umul_with_overflow<u32:4>(u4:2, u3:7), (false, u4:14));
    assert_eq(umul_with_overflow<u32:4>(u4:5, u3:3), (false, u4:15));
    assert_eq(umul_with_overflow<u32:4>(u4:4, u3:4), (true, u4:0));
    assert_eq(umul_with_overflow<u32:4>(u4:9, u3:2), (true, u4:2));
    assert_eq(umul_with_overflow<u32:4>(u4:15, u3:7), (true, u4:9));

    for (i, ()): (u32, ()) in u32:0..u32:7 {
        for (j, ()): (u32, ()) in u32:0..u32:15 {
            let result = i * j;
            let overflow = result > u32:15;

            assert_eq(umul_with_overflow<u32:4>(i as u3, j as u4), (overflow, result as u4))
        }(())
    }(());
}

fn is_unsigned_msb_set<N: u32>(x: uN[N]) -> bool { x[-1:] }

#[test]
fn is_unsigned_msb_set_test() {
    assert_eq(false, is_unsigned_msb_set(u8:0));
    assert_eq(false, is_unsigned_msb_set(u8:1));
    assert_eq(false, is_unsigned_msb_set(u8:127));
    assert_eq(true, is_unsigned_msb_set(u8:128));
    assert_eq(true, is_unsigned_msb_set(u8:129));
}
