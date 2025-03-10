// Copyright 2021 The XLS Authors
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

#include "xls/dslx/type_system/typecheck.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/error_printer.h"
#include "xls/dslx/error_test_utils.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/typecheck_test_helpers.h"

namespace xls::dslx {
namespace {

using status_testing::StatusIs;
using testing::HasSubstr;

TEST(TypecheckErrorTest, SendInFunction) {
  std::string_view text = R"(
fn f(tok: token, output_r: chan<u8> in, expected: u8) -> token {
  let (tok, value) = recv(tok, output_r);
  tok
}
)";
  EXPECT_THAT(Typecheck(text),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Cannot recv() outside of a proc")));
}

TEST(TypecheckErrorTest, ParametricWrongArgCount) {
  std::string_view text = R"(
fn id<N: u32>(x: bits[N]) -> bits[N] { x }
fn f() -> u32 { id(u8:3, u8:4) }
)";
  EXPECT_THAT(
      Typecheck(text),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Expected 1 parameter(s) but got 2 argument(s)")));
}

TEST(TypecheckErrorTest, ParametricTooManyExplicitSupplied) {
  std::string_view text = R"(
fn id<X: u32>(x: bits[X]) -> bits[X] { x }
fn main() -> u32 { id<u32:32, u32:64>(u32:5) }
)";
  EXPECT_THAT(
      Typecheck(text),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Too many parametric values supplied; limit: 1 given: 2")));
}

TEST(TypecheckErrorTest, ReturnTypeMismatch) {
  EXPECT_THAT(
      Typecheck("fn f(x: bits[3], y: bits[4]) -> bits[5] { y }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("uN[4] vs uN[5]: Return type of function body")));
}

TEST(TypecheckTest, Identity) {
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u32 { x }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: bits[3], y: bits[4]) -> bits[3] { x }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: bits[3], y: bits[4]) -> bits[4] { y }"));
}

TEST(TypecheckTest, TokenIdentity) {
  XLS_EXPECT_OK(Typecheck("fn f(x: token) -> token { x }"));
}

TEST(TypecheckTest, Unit) {
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> () { () }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) { () }"));
}

TEST(TypecheckTest, Arithmetic) {
  // Simple add.
  XLS_EXPECT_OK(Typecheck("fn f(x: u32, y: u32) -> u32 { x + y }"));
  // Wrong return type (implicitly unit).
  EXPECT_THAT(
      Typecheck("fn f(x: u32, y: u32) { x + y }"),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("uN[32] vs ()")));
  // Wrong return type (implicitly unit).
  EXPECT_THAT(
      Typecheck(R"(
      fn f<N: u32>(x: bits[N], y: bits[N]) { x + y }
      fn g() -> u64 { f(u64:5, u64:5) }
      )"),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("uN[64] vs ()")));
  // Mixing widths not permitted.
  EXPECT_THAT(Typecheck("fn f(x: u32, y: bits[4]) { x + y }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[32] vs uN[4]")));
  // Parametric same-width is ok!
  XLS_EXPECT_OK(
      Typecheck("fn f<N: u32>(x: bits[N], y: bits[N]) -> bits[N] { x + y }"));
}

TEST(TypecheckTest, Unary) {
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u32 { !x }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u32 { -x }"));
}

TEST(TypecheckTest, Let) {
  XLS_EXPECT_OK(Typecheck("fn f() -> u32 { let x: u32 = u32:2; x }"));
  EXPECT_THAT(Typecheck(
                  R"(fn f() -> u32 {
        let x: u32 = u32:2;
        let y: bits[4] = bits[4]:3;
        y
      }
      )"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[4] vs uN[32]")));
  XLS_EXPECT_OK(Typecheck(
      "fn f() -> u32 { let (x, y): (u32, bits[4]) = (u32:2, bits[4]:3); x }"));
}

TEST(TypecheckTest, LetBadRhs) {
  EXPECT_THAT(
      Typecheck(
          R"(fn f() -> bits[2] {
          let (x, (y, (z,))): (u32, (bits[4], (bits[2],))) = (u32:2, bits[4]:3);
          z
        })"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("did not match inferred type of right hand side")));
}

TEST(TypecheckTest, ParametricInvocation) {
  std::string program = R"(
fn p<N: u32>(x: bits[N]) -> bits[N] { x + bits[N]:1 }
fn f() -> u32 { p(u32:3) }
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckTest, ParametricInvocationWithTuple) {
  std::string program = R"(
fn p<N: u32>(x: bits[N]) -> (bits[N], bits[N]) { (x, x) }
fn f() -> (u32, u32) { p(u32:3) }
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckTest, DoubleParametricInvocation) {
  std::string program = R"(
fn p<N: u32>(x: bits[N]) -> bits[N] { x + bits[N]:1 }
fn o<M: u32>(x: bits[M]) -> bits[M] { p(x) }
fn f() -> u32 { o(u32:3) }
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckTest, ParametricPlusGlobal) {
  std::string program = R"(
const GLOBAL = u32:4;
fn p<N: u32>() -> bits[N+GLOBAL] { bits[N+GLOBAL]:0 }
fn f() -> u32 { p<u32:28>() }
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckTest, ParametricStructInstantiatedByGlobal) {
  std::string program = R"(
struct MyStruct<WIDTH: u32> {
  f: bits[WIDTH]
}
fn p<FIELD_WIDTH: u32>(s: MyStruct<FIELD_WIDTH>) -> u15 {
  s.f
}
const GLOBAL = u32:15;
fn f(s: MyStruct<GLOBAL>) -> u15 { p(s) }
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckTest, TopLevelConstTypeMismatch) {
  std::string program = R"(
const GLOBAL: u64 = u32:4;
)";
  EXPECT_THAT(
      Typecheck(program),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("uN[64] vs uN[32]: Constant definition's annotated "
                         "type did not match its expression's type")));
}

TEST(TypecheckTest, TopLevelConstTypeMatch) {
  std::string program = R"(
const GLOBAL: u32 = u32:4;
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckErrorTest, ParametricIdentifierLtValue) {
  std::string program = R"(
fn p<N: u32>(x: bits[N]) -> bits[N] { x }

fn f() -> bool { p < u32:42 }
)";
  EXPECT_THAT(Typecheck(program),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Name 'p' is a parametric function, but it is "
                                 "not being invoked")));
}

TEST(TypecheckTest, MapOfParametric) {
  std::string program = R"(
fn p<N: u32>(x: bits[N]) -> bits[N] { x }

fn f() -> u32[3] {
  map(u32[3]:[1, 2, 3], p)
}
)";
  XLS_EXPECT_OK(Typecheck(program));
}

TEST(TypecheckErrorTest, ParametricInvocationConflictingArgs) {
  std::string program = R"(
fn id<N: u32>(x: bits[N], y: bits[N]) -> bits[N] { x }
fn f() -> u32 { id(u8:3, u32:5) }
)";
  EXPECT_THAT(Typecheck(program), StatusIs(absl::StatusCode::kInvalidArgument,
                                           HasSubstr("saw: 8; then: 32")));
}

TEST(TypecheckErrorTest, ParametricWrongKind) {
  std::string program = R"(
fn id<N: u32>(x: bits[N]) -> bits[N] { x }
fn f() -> u32 { id((u8:3,)) }
)";
  EXPECT_THAT(Typecheck(program), StatusIs(absl::StatusCode::kInvalidArgument,
                                           HasSubstr("different kinds")));
}

TEST(TypecheckErrorTest, ParametricWrongNumberOfDims) {
  std::string program = R"(
fn id<N: u32, M: u32>(x: bits[N][M]) -> bits[N][M] { x }
fn f() -> u32 { id(u32:42) }
)";
  EXPECT_THAT(
      Typecheck(program),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("types are different kinds (array vs ubits)")));
}

TEST(TypecheckErrorTest, RecursionCausesError) {
  std::string program = "fn f(x: u32) -> u32 { f(x) }";
  EXPECT_THAT(Typecheck(program),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Recursion of function `f` detected")));
}

TEST(TypecheckErrorTest, ParametricRecursionCausesError) {
  std::string program = R"(
fn f<X: u32>(x: bits[X]) -> u32 { f(x) }
fn g() -> u32 { f(u32: 5) }
)";
  EXPECT_THAT(Typecheck(program),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Recursion of function `f` detected")));
}

TEST(TypecheckErrorTest, HigherOrderRecursionCausesError) {
  std::string program = R"(
fn h<Y: u32>(y: bits[Y]) -> bits[Y] { h(y) }
fn g() -> u32[3] {
    let x0 = u32[3]:[0, 1, 2];
    map(x0, h)
}
)";
  EXPECT_THAT(Typecheck(program),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Recursion of function `h` detected")));
}

TEST(TypecheckErrorTest, InvokeWrongArg) {
  std::string program = R"(
fn id_u32(x: u32) -> u32 { x }
fn f(x: u8) -> u8 { id_u32(x) }
)";
  EXPECT_THAT(
      Typecheck(program),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Mismatch between parameter and argument types")));
}

TEST(TypecheckErrorTest, BadTupleType) {
  std::string program = R"(
fn f() -> u32 {
  let (a, b, c): (u32, u32) = (u32:1, u32:2, u32:3);
  a
}
)";
  EXPECT_THAT(
      Typecheck(program),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Annotated type did not match inferred type")));
}

TEST(TypecheckTest, LogicalAndOfComparisons) {
  XLS_EXPECT_OK(Typecheck("fn f(a: u8, b: u8) -> bool { a == b }"));
  XLS_EXPECT_OK(Typecheck(
      "fn f(a: u8, b: u8, c: u32, d: u32) -> bool { a == b && c == d }"));
}

TEST(TypecheckTest, Typedef) {
  XLS_EXPECT_OK(Typecheck(R"(
type MyTypeAlias = (u32, u8);
fn id(x: MyTypeAlias) -> MyTypeAlias { x }
fn f() -> MyTypeAlias { id((u32:42, u8:127)) }
)"));
}

TEST(TypecheckTest, For) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32 {
  for (i, accum): (u32, u32) in range(u32:0, u32:3) {
    let new_accum: u32 = accum + i;
    new_accum
  }(u32:0)
})"));
}

TEST(TypecheckTest, ForNoAnnotation) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32 {
  for (i, accum) in range(u32:0, u32:3) {
    accum
  }(u32:0)
})"));
}

TEST(TypecheckTest, ForWildcardIvar) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32 {
  for (_, accum) in range(u32:0, u32:3) {
    accum
  }(u32:0)
})"));
}

TEST(TypecheckTest, ConstAssertParametricOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn p<N: u32>() -> u32 {
  const_assert!(N == u32:42);
  N
}
fn main() -> u32 {
  p<u32:42>()
})"));
}

TEST(TypecheckTest, ConstAssertViaConstBindings) {
  XLS_EXPECT_OK(Typecheck(R"(
fn main() -> () {
  const M = u32:1;
  const N = u32:2;
  const O = M + N;
  const_assert!(O == u32:3);
  ()
})"));
}

TEST(TypecheckTest, ConstAssertCallFunction) {
  XLS_EXPECT_OK(Typecheck(R"(
fn is_mol(x: u32) -> bool {
  x == u32:42
}
fn p<N: u32>() -> () {
  const_assert!(is_mol(N));
  ()
}
fn main() -> () {
  p<u32:42>()
})"));
}

TEST(TypecheckErrorTest, ConstAssertFalse) {
  EXPECT_THAT(Typecheck(R"(
fn main() -> () {
  const_assert!(false);
})"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("const_assert! failure: `false`")));
}

TEST(TypecheckErrorTest, ConstAssertFalseExpr) {
  EXPECT_THAT(Typecheck(R"(
fn main() -> () {
  const_assert!(u32:2 + u32:3 != u32:5);
})"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("const_assert! failure")));
}

TEST(TypecheckErrorTest, ConstAssertNonConstexpr) {
  EXPECT_THAT(Typecheck(R"(
fn main(p: u32) -> () {
  const_assert!(p == u32:42);
})"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("const_assert! expression is not constexpr")));
}

TEST(TypecheckErrorTest, FitsInTypeSN0) {
  EXPECT_THAT(Typecheck(R"(
fn main() -> sN[0] {
  sN[0]:0xffff_ffff_ffff_ffff_ffff
})"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Value '0xffff_ffff_ffff_ffff_ffff' does not "
                                 "fit in the bitwidth of a sN[0]")));
}

TEST(TypecheckErrorTest, ParametricBindArrayToTuple) {
  EXPECT_THAT(Typecheck(R"(
fn p<N: u32>(x: (uN[N], uN[N])) -> uN[N] { x.0 }

fn main() -> u32 {
  p(u32[2]:[0, 1])
})"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Parameter 0 and argument types are different "
                                 "kinds (tuple vs array)")));
}

TEST(TypecheckErrorTest, ParametricBindNested) {
  EXPECT_THAT(
      Typecheck(R"(
fn p<N: u32>(x: (u32, u64)[N]) -> u32 { x[0].0 }

fn main() -> u32 {
  p(u32[1][1]:[[u32:0]])
})"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("(uN[32], uN[64]) vs uN[32][1]: expected argument "
                         "kind 'array' to match parameter kind 'tuple'")));
}

TEST(TypecheckTest, ForBuiltinInBody) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32 {
  for (i, accum): (u32, u32) in range(u32:0, u32:3) {
    trace!(accum)
  }(u32:0)
})"));
}

TEST(TypecheckTest, ForNestedBindings) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f(x: u32) -> (u32, u8) {
  for (i, (x, y)): (u32, (u32, u8)) in range(u32:0, u32:3) {
    (x, y)
  }((x, u8:42))
}
)"));
}

TEST(TypecheckTest, ForWithBadTypeTree) {
  EXPECT_THAT(
      Typecheck(R"(
fn f(x: u32) -> (u32, u8) {
  for (i, (x, y)): (u32, u8) in range(u32:0, u32:3) {
    (x, y)
  }((x, u8:42))
})"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("(uN[32], uN[8]) vs (uN[32], (uN[32], uN[8])): For-loop "
                    "annotated type did not match inferred type.")));
}

TEST(TypecheckTest, DerivedExprTypeMismatch) {
  EXPECT_THAT(
      Typecheck(R"(
fn p<X: u32, Y: bits[4] = {X+X}>(x: bits[X]) -> bits[X] { x }
fn f() -> u32 { p(u32:3) }
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "Annotated type of derived parametric value did not match")));
}

TEST(TypecheckTest, ParametricExpressionInBitsReturnType) {
  XLS_EXPECT_OK(Typecheck(R"(
fn parametric<X: u32>() -> bits[X * u32:4] {
  type Ret = bits[X * u32:4];
  Ret:0
}
fn main() -> bits[4] { parametric<u32:1>() }
)"));
}

TEST(TypecheckTest, ParametricInstantiationVsArgOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn parametric<X: u32 = {u32:5}> (x: bits[X]) -> bits[X] { x }
fn main() -> bits[5] { parametric(u5:1) }
)"));
}

TEST(TypecheckTest, ParametricInstantiationVsArgError) {
  EXPECT_THAT(
      Typecheck(R"(
fn foo<X: u32 = {u32:5}>(x: bits[X]) -> bits[X] { x }
fn bar() -> bits[10] { foo(u5:1) + foo(u10: 1) }
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Inconsistent parametric instantiation of function, "
                         "first saw X = u32:10; then saw X = u32:5 = u32:5")));
}

TEST(TypecheckTest, ParametricInstantiationVsBodyOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn parametric<X: u32 = {u32:5}>() -> bits[5] { bits[X]:1 + bits[5]:1 }
fn main() -> bits[5] { parametric() }
)"));
}

TEST(TypecheckTest, ParametricInstantiationVsBodyError) {
  EXPECT_THAT(Typecheck(R"(
fn foo<X: u32 = {u32:5}>() -> bits[10] { bits[X]:1 + bits[10]:1 }
fn bar() -> bits[10] { foo() }
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[5] vs uN[10]: Could not deduce type for "
                                 "binary operation '+'")));
}

TEST(TypecheckTest, ParametricInstantiationVsReturnOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn parametric<X: u32 = {u32: 5}>() -> bits[5] { bits[X]: 1 }
fn main() -> bits[5] { parametric() }
)"));
}

TEST(TypecheckTest, ParametricInstantiationVsReturnError) {
  EXPECT_THAT(
      Typecheck(R"(
fn foo<X: u32 = {u32: 5}>() -> bits[10] { bits[X]: 1 }
fn bar() -> bits[10] { foo() }
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Return type of function body for 'foo' did not match")));
}

TEST(TypecheckTest, ParametricIndirectInstantiationVsArgOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn foo<X: u32>(x1: bits[X], x2: bits[X]) -> bits[X] { x1 + x2 }
fn fazz<Y: u32>(y: bits[Y]) -> bits[Y] { foo(y, y + bits[Y]: 1) }
fn bar() -> bits[10] { fazz(u10: 1) }
)"));
}

TEST(TypecheckTest, ParametricInstantiationVsArgError2) {
  EXPECT_THAT(
      Typecheck(R"(
fn foo<X: u32>(x1: bits[X], x2: bits[X]) -> bits[X] { x1 + x2 }
fn fazz<Y: u32>(y: bits[Y]) -> bits[Y] { foo(y, y++y) }
fn bar() -> bits[10] { fazz(u10: 1) }
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Parametric value X was bound to different values")));
}

TEST(TypecheckTest, ParametricIndirectInstantiationVsBodyOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn foo<X: u32, R: u32 = {X + X}>(x: bits[X]) -> bits[R] {
  let a = bits[R]: 5;
  x++x + a
}
fn fazz<Y: u32, T: u32 = {Y + Y}>(y: bits[Y]) -> bits[T] { foo(y) }
fn bar() -> bits[10] { fazz(u5:1) }
)"));
}

TEST(TypecheckTest, ParametricIndirectInstantiationVsBodyError) {
  EXPECT_THAT(Typecheck(R"(
fn foo<X: u32, D: u32 = {X + X}>(x: bits[X]) -> bits[X] {
  let a = bits[D]:5;
  x + a
}
fn fazz<Y: u32>(y: bits[Y]) -> bits[Y] { foo(y) }
fn bar() -> bits[5] { fazz(u5:1) })"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[5] vs uN[10]: Could not deduce type for "
                                 "binary operation '+'")));
}

TEST(TypecheckTest, ParametricIndirectInstantiationVsReturnOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn foo<X: u32, R: u32 = {X + X}>(x: bits[X]) -> bits[R] { x++x }
fn fazz<Y: u32, T: u32 = {Y + Y}>(y: bits[Y]) -> bits[T] { foo(y) }
fn bar() -> bits[10] { fazz(u5:1) }
)"));
}

TEST(TypecheckTest, ParametricIndirectInstantiationVsReturnError) {
  EXPECT_THAT(
      Typecheck(R"(
fn foo<X: u32, R: u32 = {X + X}>(x: bits[X]) -> bits[R] { x * x }
fn fazz<Y: u32, T: u32 = {Y + Y}>(y: bits[Y]) -> bits[T] { foo(y) }
fn bar() -> bits[10] { fazz(u5:1) }
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Return type of function body for 'foo' did not match")));
}

TEST(TypecheckTest, ParametricDerivedInstantiationVsArgOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn foo<X: u32, Y: u32 = {X + X}>(x: bits[X], y: bits[Y]) -> bits[X] { x }
fn bar() -> bits[5] { foo(u5:1, u10: 2) }
)"));
}

TEST(TypecheckTest, ParametricDerivedInstantiationVsArgError) {
  EXPECT_THAT(
      Typecheck(R"(
fn foo<X: u32, Y: u32 = {X + X}>(x: bits[X], y: bits[Y]) -> bits[X] { x }
fn bar() -> bits[5] { foo(u5:1, u11: 2) }
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Inconsistent parametric instantiation of function, first "
                    "saw Y = u32:11; then saw Y = X + X = u32:10")));
}

TEST(TypecheckTest, ParametricDerivedInstantiationVsBodyOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn foo<W: u32, Z: u32 = {W + W}>(w: bits[W]) -> bits[1] {
    let val: bits[Z] = w++w + bits[Z]: 5;
    and_reduce(val)
}
fn bar() -> bits[1] { foo(u5: 5) + foo(u10: 10) }
)"));
}

TEST(TypecheckTest, ParametricDerivedInstantiationVsBodyError) {
  EXPECT_THAT(Typecheck(R"(
fn foo<W: u32, Z: u32 = {W + W}>(w: bits[W]) -> bits[1] {
  let val: bits[Z] = w + w;
  and_reduce(val)
}
fn bar() -> bits[1] { foo(u5:5) }
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[10] vs uN[5]")));
}

TEST(TypecheckTest, ParametricDerivedInstantiationVsReturnOk) {
  XLS_EXPECT_OK(Typecheck(R"(
fn double<X: u32, Y: u32 = {X + X}> (x: bits[X]) -> bits[Y] { x++x }
fn foo<W: u32, Z: u32 = {W + W}> (w: bits[W]) -> bits[Z] { double(w) }
fn bar() -> bits[10] { foo(u5:1) }
)"));
}

TEST(TypecheckTest, ParametricDerivedInstantiationVsReturnError) {
  EXPECT_THAT(
      Typecheck(R"(
fn double<X: u32, Y: u32 = {X + X}>(x: bits[X]) -> bits[Y] { x + x }
fn foo<W: u32, Z: u32 = {W + W}>(w: bits[W]) -> bits[Z] { double(w) }
fn bar() -> bits[10] { foo(u5:1) }
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr(
                   "Return type of function body for 'double' did not match")));
}

TEST(TypecheckTest, ParametricDerivedInstantiationViaFnCall) {
  XLS_EXPECT_OK(Typecheck(R"(
fn double(n: u32) -> u32 { n * u32: 2 }
fn foo<W: u32, Z: u32 = {double(W)}>(w: bits[W]) -> bits[Z] { w++w }
fn bar() -> bits[10] { foo(u5:1) }
)"));
}

TEST(TypecheckTest, ParametricFnNotAlwaysPolymorphic) {
  EXPECT_THAT(Typecheck(R"(
fn foo<X: u32>(x: bits[X]) -> u1 {
  let non_polymorphic = x + u5: 1;
  u1:0
}
fn bar() -> bits[1] {
  foo(u5:5) ^ foo(u10:5)
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[10] vs uN[5]: Could not deduce type for "
                                 "binary operation '+'")));
}

TEST(TypecheckTest, ParametricWidthSliceStartError) {
  EXPECT_THAT(Typecheck(R"(
fn make_u1<N: u32>(x: bits[N]) -> u1 {
  x[4 +: bits[1]]
}
fn bar() -> bits[1] {
  make_u1(u10:5) ^ make_u1(u2:1)
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Cannot fit slice start 4 in 2 bits")));
}

TEST(TypecheckTest, BitSliceOnParametricWidth) {
  XLS_EXPECT_OK(Typecheck(R"(
fn get_middle_bits<N: u32, R: u32 = {N - u32:2}>(x: bits[N]) -> bits[R] {
  x[1:-1]
}

fn caller() {
  let x1: u2 = get_middle_bits(u4:15);
  let x2: u4 = get_middle_bits(u6:63);
  ()
}
)"));
}

TEST(TypecheckErrorTest, ParametricMapNonPolymorphic) {
  EXPECT_THAT(Typecheck(R"(
fn add_one<N: u32>(x: bits[N]) -> bits[N] { x + bits[5]:1 }

fn main() {
  let arr = [u5:1, u5:2, u5:3];
  let mapped_arr = map(arr, add_one);
  let type_error = add_one(u6:1);
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[6] vs uN[5]")));
}

TEST(TypecheckErrorTest, LetBindingInferredDoesNotMatchAnnotation) {
  EXPECT_THAT(Typecheck(R"(
fn f() -> u32 {
  let x: u32 = bits[4]:7;
  x
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Annotated type did not match inferred type "
                                 "of right hand side")));
}

TEST(TypecheckErrorTest, CoverBuiltinWrongArgc) {
  EXPECT_THAT(
      Typecheck(R"(
fn f() -> () {
  cover!()
}
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Invalid number of arguments passed to 'cover!'")));
}

TEST(TypecheckErrorTest, MapBuiltinWrongArgc0) {
  EXPECT_THAT(
      Typecheck(R"(
fn f() {
  map()
}
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "Expected 2 arguments to `map` builtin but got 0 argument(s)")));
}

TEST(TypecheckErrorTest, MapBuiltinWrongArgc1) {
  EXPECT_THAT(
      Typecheck(R"(
fn f(x: u32[3]) -> u32[3] {
  map(x)
}
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "Expected 2 arguments to `map` builtin but got 1 argument(s)")));
}

TEST(TypecheckTest, UpdateBuiltin) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32[3] {
  let x: u32[3] = u32[3]:[0, 1, 2];
  update(x, u32:1, u32:3)
}
)"));
}

TEST(TypecheckTest, SliceBuiltin) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32[3] {
  let x: u32[2] = u32[2]:[0, 1];
  slice(x, u32:0, u32[3]:[0, 0, 0])
}
)"));
}

TEST(TypecheckTest, EnumerateBuiltin) {
  XLS_EXPECT_OK(Typecheck(R"(
type MyTup = (u32, u2);
fn f(x: u2[7]) -> MyTup[7] {
  enumerate(x)
}
)"));
}

TEST(TypecheckTest, TernaryEmptyBlocks) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f(p: bool) -> () {
  if p { } else { }
}
)"));
}

TEST(TypecheckTest, TernaryNonBoolean) {
  EXPECT_THAT(
      Typecheck(R"(
fn f(x: u32) -> u32 {
  if x { u32:42 } else { u32:64 }
}
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Test type for conditional expression is not \"bool\"")));
}

TEST(TypecheckTest, AddWithCarryBuiltin) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f(x: u32) -> (u1, u32) {
  add_with_carry(x, x)
}
)"));
}

TEST(TypecheckErrorTest, ArraySizeOfBitsType) {
  EXPECT_THAT(
      Typecheck(R"(
fn f(x: u32) -> u32 { array_size(x) }
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "Want argument 0 to 'array_size' to be an array; got uN[32]")));
}

TEST(TypecheckTest, ArraySizeOfStructs) {
  XLS_EXPECT_OK(Typecheck(R"(
struct MyStruct {}
fn f(x: MyStruct[5]) -> u32 { array_size(x) }
)"));
}

TEST(TypecheckTest, ArraySizeOfNil) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f(x: ()[5]) -> u32 { array_size(x) }
)"));
}

TEST(TypecheckTest, ArraySizeOfTupleArray) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f(x: (u32, u64)[5]) -> u32 { array_size(x) }
)"));
}

TEST(TypecheckTest, BitSliceUpdateBuiltIn) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f(x: u32, y: u17, z: u15) -> u32 {
  bit_slice_update(x, y, z)
}
)"));
}

TEST(TypecheckErrorTest, UpdateIncompatibleValue) {
  EXPECT_THAT(Typecheck(R"(
fn f(x: u32[5]) -> u32[5] {
  update(x, u32:1, u8:0)
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[32] to match argument 2 type uN[8]")));
}

TEST(TypecheckErrorTest, UpdateMultidimIndex) {
  EXPECT_THAT(Typecheck(R"(
fn f(x: u32[6][5], i: u32[2]) -> u32[6][5] {
  update(x, i, u32[6]:[0, ...])
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Want argument 1 to be unsigned bits")));
}

TEST(TypecheckTest, MissingAnnotation) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u32 {
  let x = u32:2;
  x + x
}
)"));
}

TEST(TypecheckTest, Index) {
  XLS_EXPECT_OK(Typecheck("fn f(x: uN[32][4]) -> u32 { x[u32:0] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32[5], i: u8) -> u32 { x[i] }"));
  EXPECT_THAT(
      Typecheck("fn f(x: u32, i: u8) -> u32 { x[i] }"),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("not an array")));
  EXPECT_THAT(Typecheck("fn f(x: u32[5], i: u8[5]) -> u32 { x[i] }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("not (scalar) bits")));
}

TEST(TypecheckTest, OutOfRangeNumber) {
  XLS_EXPECT_OK(Typecheck("fn f() -> u8 { u8:255 }"));
  XLS_EXPECT_OK(Typecheck("fn f() -> s8 { s8:-1 }"));
  EXPECT_THAT(
      Typecheck("fn f() -> u8 { u8:256 }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Value '256' does not fit in the bitwidth of a uN[8]")));
  EXPECT_THAT(
      Typecheck("fn f() -> s8 { s8:256 }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Value '256' does not fit in the bitwidth of a sN[8]")));
}

TEST(TypecheckTest, OutOfRangeNumberInConstantArray) {
  EXPECT_THAT(
      Typecheck("fn f() -> u8[3] { u8[3]:[1, 2, 256] }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Value '256' does not fit in the bitwidth of a uN[8]")));
}

TEST(TypecheckErrorTest, ConstantArrayEmptyMembersWrongCountVsDecl) {
  EXPECT_THAT(Typecheck("const MY_ARRAY = u32[1]:[];"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[32][1] Annotated array size 1 does not "
                                 "match inferred array size 0.")));
}

TEST(TypecheckTest, MatchNoArms) {
  EXPECT_THAT(Typecheck("fn f(x: u8) -> u8 { let _ = match x {}; x }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Match construct has no arms")));
}

TEST(TypecheckTest, MatchArmMismatch) {
  EXPECT_THAT(
      Typecheck("fn f(x: u8) -> u8 { match x { u8:0 => u8:3, _ => u3:3 } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("match arm did not have the same type")));
}

TEST(TypecheckTest, ArrayInconsistency) {
  EXPECT_THAT(Typecheck(R"(
type Foo = (u8, u32);
fn f() -> Foo {
  let xs = Foo[2]:[(u8:0, u32:1), u32:2];
  xs[u32:1]
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("vs uN[32]: Array member did not have same "
                                 "type as other members.")));
}

TEST(TypecheckTest, ArrayOfConsts) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> u4 {
  let a: u4 = u4:1;
  let my_array = [a];
  a
}
)"));
}

TEST(TypecheckTest, EnumIdentity) {
  XLS_EXPECT_OK(Typecheck(R"(
enum MyEnum : u1 {
  A = false,
  B = true,
}
fn f(x: MyEnum) -> MyEnum { x }
)"));
}

TEST(TypecheckTest, ImplicitWidthEnum) {
  XLS_EXPECT_OK(Typecheck(R"(
enum MyEnum {
  A = false,
  B = true,
}
)"));
}

TEST(TypecheckTest, ImplicitWidthEnumFromConstexprs) {
  XLS_EXPECT_OK(Typecheck(R"(
const X = u8:42;
const Y = u8:64;
enum MyEnum {
  A = X,
  B = Y,
}
)"));
}

TEST(TypecheckTest, ImplicitWidthEnumWithConstexprAndBareLiteral) {
  XLS_EXPECT_OK(Typecheck(R"(
const X = u8:42;
enum MyEnum {
  A = 64,
  B = X,
}

const EXTRACTED_A = MyEnum::A as u8;
)"));
}

TEST(TypecheckTest, ImplicitWidthEnumFromConstexprsMismatch) {
  EXPECT_THAT(Typecheck(R"(
const X = u7:42;
const Y = u8:64;
enum MyEnum {
  A = X,
  B = Y,
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[7] vs uN[8]: Inconsistent member types in "
                                 "enum definition.")));
}

TEST(TypecheckTest, ImplicitWidthEnumMismatch) {
  EXPECT_THAT(
      Typecheck(R"(
enum MyEnum {
  A = u1:0,
  B = u2:1,
}
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "uN[1] vs uN[2]: Inconsistent member types in enum definition")));
}

TEST(TypecheckTest, ExplicitWidthEnumMismatch) {
  EXPECT_THAT(Typecheck(R"(
enum MyEnum : u2 {
  A = u1:0,
  B = u1:1,
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[1] vs uN[2]: Enum-member type did not "
                                 "match the enum's underlying type")));
}

TEST(TypecheckTest, ArrayEllipsis) {
  XLS_EXPECT_OK(Typecheck("fn main() -> u8[2] { u8[2]:[0, ...] }"));
}

TEST(TypecheckErrorTest, ArrayEllipsisNoTrailingElement) {
  EXPECT_THAT(
      Typecheck("fn main() -> u8[2] { u8[2]:[...] }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Array cannot have an ellipsis without an element to "
                         "repeat; please add at least one element")));
}

TEST(TypecheckTest, BadArrayAddition) {
  EXPECT_THAT(Typecheck(R"(
fn f(a: bits[32][4], b: bits[32][4]) -> bits[32][4] {
  a + b
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Binary operations can only be applied")));
}

TEST(TypecheckTest, OneHotBadPrioType) {
  EXPECT_THAT(
      Typecheck(R"(
fn f(x: u7, prio: u2) -> u8 {
  one_hot(x, prio)
}
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Expected argument 1 to 'one_hot' to be a u1; got uN[2]")));
}

TEST(TypecheckTest, OneHotSelOfSigned) {
  XLS_EXPECT_OK(Typecheck(R"(
fn f() -> s4 {
  let a: s4 = s4:1;
  let b: s4 = s4:2;
  let s: u2 = u2:0b01;
  one_hot_sel(s, [a, b])
}
)"));
}

TEST(TypecheckTest, OverlargeEnumValue) {
  EXPECT_THAT(
      Typecheck(R"(
enum Foo : u1 {
  A = 0,
  B = 1,
  C = 2,
}
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Value '2' does not fit in the bitwidth of a uN[1]")));
}

TEST(TypecheckTest, CannotAddEnums) {
  EXPECT_THAT(
      Typecheck(R"(
enum Foo : u2 {
  A = 0,
  B = 1,
}
fn f() -> Foo {
  Foo::A + Foo::B
}
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Cannot use '+' on values with enum type Foo")));
}

TEST(TypecheckTest, SlicesWithMismatchedTypes) {
  EXPECT_THAT(Typecheck("fn f(x: u8) -> u8 { x[s4:0 : s5:1] }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Slice limit type (sN[5]) did not match")));
}

TEST(TypecheckTest, SliceWithOutOfRangeLimit) {
  EXPECT_THAT(Typecheck("fn f(x: uN[128]) -> uN[128] { x[s4:0 :] }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Slice limit does not fit in index type")));
  EXPECT_THAT(Typecheck("fn f(x: uN[8]) -> uN[8] { x[s3:0 :] }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Slice limit does not fit in index type")));
}

TEST(TypecheckTest, SliceWithNonS32LiteralBounds) {
  // overlarge value in start
  EXPECT_THAT(
      Typecheck("fn f(x: uN[128]) -> uN[128] { x[40000000000000000000:] }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Value '40000000000000000000' does not fit in the "
                         "bitwidth of a sN[32]")));
  // overlarge value in limit
  EXPECT_THAT(
      Typecheck("fn f(x: uN[128]) -> uN[128] { x[:40000000000000000000] }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Value '40000000000000000000' does not fit in the "
                         "bitwidth of a sN[32]")));
}

TEST(TypecheckTest, WidthSlices) {
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> bits[0] { x[0+:bits[0]] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u2 { x[32+:u2] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u1 { x[31+:u1] }"));
}

TEST(TypecheckTest, WidthSliceNegativeStartNumber) {
  // Start literal is treated as unsigned.
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u1 { x[-1+:u1] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u2 { x[-1+:u2] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u3 { x[-2+:u3] }"));
}

TEST(TypecheckTest, WidthSliceEmptyStartNumber) {
  // Start literal is treated as unsigned.
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u31 { x[:-1] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u30 { x[:-2] }"));
  XLS_EXPECT_OK(Typecheck("fn f(x: u32) -> u29 { x[:-3] }"));
}

TEST(TypecheckTest, WidthSliceSignedStart) {
  // We reject signed start literals.
  EXPECT_THAT(
      Typecheck("fn f(start: s32, x: u32) -> u3 { x[start+:u3] }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Start index for width-based slice must be unsigned")));
}

TEST(TypecheckTest, WidthSliceTupleStart) {
  EXPECT_THAT(
      Typecheck("fn f(start: (s32), x: u32) -> u3 { x[start+:u3] }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Start expression for width slice must be bits typed")));
}

TEST(TypecheckTest, WidthSliceTupleSubject) {
  EXPECT_THAT(Typecheck("fn f(start: s32, x: (u32)) -> u3 { x[start+:u3] }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Value to slice is not of 'bits' type")));
}

TEST(TypecheckTest, OverlargeWidthSlice) {
  EXPECT_THAT(Typecheck("fn f(x: u32) -> u33 { x[0+:u33] }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Slice type must have <= original number of "
                                 "bits; attempted slice from 32 to 33 bits.")));
}

TEST(TypecheckTest, BadAttributeAccessOnTuple) {
  EXPECT_THAT(Typecheck(R"(
fn main() -> () {
  let x: (u32,) = (u32:42,);
  x.a
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Expected a struct for attribute access")));
}

TEST(TypecheckTest, BadAttributeAccessOnBits) {
  EXPECT_THAT(Typecheck(R"(
fn main() -> () {
  let x = u32:42;
  x.a
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Expected a struct for attribute access")));
}

TEST(TypecheckTest, BadArrayLiteralType) {
  EXPECT_THAT(Typecheck(R"(
fn main() -> s32[2] {
  s32:[1, 2]
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Annotated type for array literal must be an "
                                 "array type; got sbits s32")));
}

TEST(TypecheckTest, CharLiteralArray) {
  XLS_EXPECT_OK(Typecheck(R"(
fn main() -> u8[3] {
  u8[3]:['X', 'L', 'S']
}
)"));
}

TEST(TypecheckTest, BadEnumRef) {
  EXPECT_THAT(
      Typecheck(R"(
enum MyEnum : u1 { A = 0, B = 1 }
fn f() -> MyEnum { MyEnum::C }
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Name 'C' is not defined by the enum MyEnum")));
}

TEST(TypecheckTest, NominalTyping) {
  // Nominal typing not structural, e.g. OtherPoint cannot be passed where we
  // want a Point, even though their members are the same.
  EXPECT_THAT(Typecheck(R"(
struct Point { x: s8, y: u32 }
struct OtherPoint { x: s8, y: u32 }
fn f(x: Point) -> Point { x }
fn g() -> Point {
  let shp = OtherPoint { x: s8:255, y: u32:1024 };
  f(shp)
}
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("struct 'Point' structure: Point { x: sN[8], "
                                 "y: uN[32] } vs struct 'OtherPoint'")));
}

TEST(TypecheckTest, ParametricWithConstantArrayEllipsis) {
  XLS_EXPECT_OK(Typecheck(R"(
fn p<N: u32>(_: bits[N]) -> u8[2] { u8[2]:[0, ...] }
fn main() -> u8[2] { p(false) }
)"));
}

TEST(TypecheckErrorTest, BadQuickcheckFunctionRet) {
  EXPECT_THAT(Typecheck(R"(
#[quickcheck]
fn f() -> u5 { u5:1 }
)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("must return a bool")));
}

TEST(TypecheckErrorTest, BadQuickcheckFunctionParametrics) {
  EXPECT_THAT(
      Typecheck(R"(
#[quickcheck]
fn f<N: u32>() -> bool { true }
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Quickchecking parametric functions is unsupported")));
}

TEST(TypecheckTest, GetAsBuiltinType) {
  constexpr std::string_view kProgram = R"(
struct Foo {
  a: u64,
  b: u1,
  c: bits[1],
  d: bits[64],
  e: sN[0],
  f: sN[1],
  g: uN[1],
  h: uN[64],
  i: uN[66],
  j: bits[32][32],
  k: u64[32],
}
)";

  auto import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule module,
      ParseAndTypecheck(kProgram, "fake_path", "MyModule", &import_data));
  StructDef* struct_def = module.module->GetStructDefs()[0];
  // The classic for-switch pattern. :)
  for (int i = 0; i < struct_def->members().size(); i++) {
    XLS_ASSERT_OK_AND_ASSIGN(
        std::optional<BuiltinType> as_builtin,
        GetAsBuiltinType(module.module, module.type_info, &import_data,
                         struct_def->members()[i].second));

    if (i == 4 || i == 8 || i == 9 || i == 10) {
      ASSERT_FALSE(as_builtin.has_value()) << "Case : " << i;
      continue;
    }

    XLS_ASSERT_OK_AND_ASSIGN(bool is_signed,
                             GetBuiltinTypeSignedness(as_builtin.value()));
    XLS_ASSERT_OK_AND_ASSIGN(int64_t bit_count,
                             GetBuiltinTypeBitCount(as_builtin.value()));
    switch (i) {
      case 0:
        EXPECT_FALSE(is_signed);
        EXPECT_EQ(bit_count, 64);
        break;
      case 1:
        EXPECT_FALSE(is_signed);
        EXPECT_EQ(bit_count, 1);
        break;
      case 2:
        EXPECT_FALSE(is_signed);
        EXPECT_EQ(bit_count, 1);
        break;
      case 3:
        EXPECT_FALSE(is_signed);
        EXPECT_EQ(bit_count, 64);
        break;
      case 5:
        EXPECT_TRUE(is_signed);
        EXPECT_EQ(bit_count, 1);
        break;
      case 6:
        EXPECT_FALSE(is_signed);
        EXPECT_EQ(bit_count, 1);
        break;
      case 7:
        EXPECT_FALSE(is_signed);
        EXPECT_EQ(bit_count, 64);
        break;
      default:
        FAIL();
    }
  }
}

TEST(TypecheckTest, NumbersAreConstexpr) {
  // Visitor to check all nodes in the below program to determine if all numbers
  // are indeed constexpr.
  class IsConstVisitor : public AstNodeVisitorWithDefault {
   public:
    explicit IsConstVisitor(TypeInfo* type_info) : type_info_(type_info) {}

    absl::Status HandleFunction(const Function* node) override {
      XLS_RETURN_IF_ERROR(node->body()->Accept(this));
      return absl::OkStatus();
    }

    absl::Status HandleLet(const Let* node) override {
      XLS_RETURN_IF_ERROR(node->rhs()->Accept(this));
      return absl::OkStatus();
    }

    absl::Status HandleNumber(const Number* node) override {
      if (!type_info_->GetConstExpr(node).ok()) {
        all_numbers_constexpr_ = false;
      }
      return absl::OkStatus();
    }

    bool all_numbers_constexpr() { return all_numbers_constexpr_; }

   private:
    bool all_numbers_constexpr_ = true;
    TypeInfo* type_info_;
  };

  constexpr std::string_view kProgram = R"(
fn main() {
  let foo = u32:0;
  let foo = u64:0x666;
  ()
}
)";

  ImportData import_data(CreateImportDataForTest());
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f,
                           tm.module->GetMemberOrError<Function>("main"));
  IsConstVisitor visitor(tm.type_info);
  XLS_ASSERT_OK(f->Accept(&visitor));
  EXPECT_TRUE(visitor.all_numbers_constexpr());
}

TEST(TypecheckTest, BasicTupleIndex) {
  XLS_EXPECT_OK(Typecheck(R"(
fn main() -> u18 {
  (u32:7, u24:6, u18:5, u12:4, u8:3).2
}
)"));
}

TEST(TypecheckTest, BasicRange) {
  constexpr std::string_view kProgram = R"(#[test]
fn main() {
  let a = u32:0..u32:4;
  let b = u32[4]:[0, 1, 2, 3];
  assert_eq(a, b)
}
)";

  XLS_EXPECT_OK(Typecheck(kProgram));
}

// Helper for struct instance based tests.
static absl::Status TypecheckStructInstance(std::string program) {
  program = R"(
struct Point {
  x: s8,
  y: u32,
}
)" + program;
  return Typecheck(program).status();
}

TEST(TypecheckStructInstanceTest, AccessMissingMember) {
  EXPECT_THAT(
      TypecheckStructInstance("fn f(p: Point) -> () { p.z }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Struct 'Point' does not have a member with name 'z'")));
}

TEST(TypecheckStructInstanceTest, WrongType) {
  EXPECT_THAT(TypecheckStructInstance(
                  "fn f() -> Point { Point { y: u8:42, x: s8:255 } }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("uN[32] vs uN[8]")));
}

TEST(TypecheckStructInstanceTest, MissingFieldX) {
  EXPECT_THAT(
      TypecheckStructInstance("fn f() -> Point { Point { y: u32:42 } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Struct instance is missing member(s): 'x'")));
}

TEST(TypecheckStructInstanceTest, MissingFieldY) {
  EXPECT_THAT(
      TypecheckStructInstance("fn f() -> Point { Point { x: s8: 255 } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Struct instance is missing member(s): 'y'")));
}

TEST(TypecheckStructInstanceTest, OutOfOrderOk) {
  XLS_EXPECT_OK(TypecheckStructInstance(
      "fn f() -> Point { Point { y: u32:42, x: s8:255 } }"));
}

TEST(TypecheckStructInstanceTest, ProvideExtraFieldZ) {
  EXPECT_THAT(
      TypecheckStructInstance(
          "fn f() -> Point { Point { x: s8:255, y: u32:42, z: u32:1024 } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Struct \'Point\' has no member \'z\', but it was "
                         "provided by this instance.")));
}

TEST(TypecheckStructInstanceTest, DuplicateFieldY) {
  EXPECT_THAT(
      TypecheckStructInstance(
          "fn f() -> Point { Point { x: s8:255, y: u32:42, y: u32:1024 } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Duplicate value seen for \'y\' in this \'Point\' "
                         "struct instance.")));
}

TEST(TypecheckStructInstanceTest, StructIncompatibleWithTupleEquivalent) {
  EXPECT_THAT(
      TypecheckStructInstance(R"(
fn f(x: (s8, u32)) -> (s8, u32) { x }
fn g() -> (s8, u32) {
  let p = Point { x: s8:255, y: u32:1024 };
  f(p)
}
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("(sN[8], uN[32]) vs struct 'Point' structure")));
}

TEST(TypecheckStructInstanceTest, SplatWithDuplicate) {
  EXPECT_THAT(
      TypecheckStructInstance(
          "fn f(p: Point) -> Point { Point { x: s8:42, x: s8:64, ..p } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Duplicate value seen for \'x\' in this \'Point\' "
                         "struct instance.")));
}

TEST(TypecheckStructInstanceTest, SplatWithExtraFieldQ) {
  EXPECT_THAT(TypecheckStructInstance(
                  "fn f(p: Point) -> Point { Point { q: u32:42, ..p } }"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Struct 'Point' has no member 'q'")));
}

TEST(TypecheckParametricStructInstanceTest, MulExprInMember) {
  const std::string_view kProgram = R"(
struct Point<N: u32> {
  x: uN[N],
  y: uN[N * u32:2]
}

fn f(p: Point<3>) -> uN[6] {
  p.y
}
)";
  XLS_EXPECT_OK(Typecheck(kProgram));
}

// TODO(https://github.com/google/xls/issues/978) Enable types other than u32 to
// be used in struct parametric instantiation.
TEST(TypecheckParametricStructInstanceTest, DISABLED_NonU32Parametric) {
  const std::string_view kProgram = R"(
struct Point<N: u5, N_U32: u32 = {N as u32}> {
  x: uN[N_U32],
}

fn f(p: Point<u5:3>) -> uN[3] {
  p.y
}
)";
  XLS_EXPECT_OK(Typecheck(kProgram));
}

// Helper for parametric struct instance based tests.
static absl::Status TypecheckParametricStructInstance(std::string program) {
  program = R"(
struct Point<N: u32, M: u32 = {N + N}> {
  x: bits[N],
  y: bits[M],
}
)" + program;
  return Typecheck(program).status();
}

TEST(TypecheckParametricStructInstanceTest, WrongDerivedType) {
  EXPECT_THAT(
      TypecheckParametricStructInstance(
          "fn f() -> Point<32, 63> { Point { x: u32:5, y: u63:255 } }"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("first saw M = u32:63; then saw M = N + N = u32:64")));
}

TEST(TypecheckParametricStructInstanceTest, TooManyParametricArgs) {
  EXPECT_THAT(
      TypecheckParametricStructInstance(
          "fn f() -> Point<u32:5, u32:10, u32:15> { Point { x: u5:5, y: "
          "u10:255 } }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Expected 2 parametric arguments for 'Point'; got 3")));
}

TEST(TypecheckParametricStructInstanceTest, OutOfOrderOk) {
  XLS_EXPECT_OK(TypecheckParametricStructInstance(
      "fn f() -> Point<32, 64> { Point { y: u64:42, x: u32:255 } }"));
}

TEST(TypecheckParametricStructInstanceTest,
     OkInstantiationInParametricFunction) {
  XLS_EXPECT_OK(TypecheckParametricStructInstance(R"(
fn f<A: u32, B: u32>(x: bits[A], y: bits[B]) -> Point<A, B> { Point { x, y } }
fn main() {
  let _ = f(u5:1, u10:2);
  let _ = f(u14:1, u28:2);
  ()
}
)"));
}

TEST(TypecheckParametricStructInstanceTest, BadReturnType) {
  EXPECT_THAT(
      TypecheckParametricStructInstance(
          "fn f() -> Point<5, 10> { Point { x: u32:5, y: u64:255 } }"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr(
              "struct 'Point' structure: Point { x: uN[32], y: uN[64] } vs "
              "struct 'Point' structure: Point { x: uN[5], y: uN[10] }")));
}

// Bad struct type-parametric instantiation in parametric function.
TEST(TypecheckParametricStructInstanceTest, BadParametricInstantiation) {
  EXPECT_THAT(
      TypecheckParametricStructInstance(R"(
fn f<A: u32, B: u32>(x: bits[A], y: bits[B]) -> Point<A, B> {
  Point { x, y }
}

fn main() {
  let _ = f(u5:1, u10:2);
  let _ = f(u14:1, u15:2);
  ()
}
)"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Inconsistent parametric instantiation of struct, first "
                    "saw M = u32:15; then saw M = N + N = u32:28")));
}

TEST(TypecheckParametricStructInstanceTest, BadParametricSplatInstantiation) {
  EXPECT_THAT(
      TypecheckParametricStructInstance(R"(
fn f<A: u32, B: u32>(x: bits[A], y: bits[B]) -> Point<A, B> {
  let p = Point { x, y };
  Point { x: (x++x), ..p }
}

fn main() {
  let _ = f(u5:1, u10:2);
  ()
}
)"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("first saw M = u32:10; then saw M = N + N = u32:20")));
}

TEST(TypecheckTest, AttrViaColonRef) {
  XLS_EXPECT_OK(Typecheck("fn f() -> u8 { u8::ZERO }"));
  XLS_EXPECT_OK(Typecheck("fn f() -> u8 { u8::MAX }"));
}

TEST(TypecheckTest, ColonRefTypeAlias) {
  XLS_EXPECT_OK(Typecheck(R"(
type MyU8 = u8;
fn f() -> u8 { MyU8::MAX }
fn g() -> u8 { MyU8::ZERO }
)"));
}

TEST(TypecheckTest, MaxAttrUsedToDefineAType) {
  XLS_EXPECT_OK(Typecheck(R"(
type MyU255 = uN[u8::MAX as u32];
fn f() -> MyU255 { uN[255]:42 }
)"));
}

TEST(TypecheckTest, ZeroAttrUsedToDefineAType) {
  XLS_EXPECT_OK(Typecheck(R"(
type MyU0 = uN[u8::ZERO as u32];
fn f() -> MyU0 { bits[0]:0 }
)"));
}

TEST(TypecheckTest, TypeAliasOfStructWithBoundParametrics) {
  XLS_EXPECT_OK(Typecheck(R"(
struct S<X: u32, Y: u32> {
  x: bits[X],
  y: bits[Y],
}
type MyS = S<3, 4>;
fn f() -> MyS { MyS{x: bits[3]:3, y: bits[4]:4 } }
)"));
}

TEST(TypecheckTest, SplatWithAllStructMembersSpecifiedGivesWarning) {
  const std::string program = R"(
struct S {
  x: u32,
  y: u32,
}
fn f(s: S) -> S { S{x: u32:4, y: u32:8, ..s} }
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, Typecheck(program));
  ASSERT_THAT(tm.warnings.warnings().size(), 1);
  std::string filename = "fake.x";
  EXPECT_EQ(tm.warnings.warnings().at(0).span,
            Span(Pos(filename, 5, 42), Pos(filename, 5, 43)));
  EXPECT_EQ(tm.warnings.warnings().at(0).message,
            "'Splatted' struct instance has all members of struct defined, "
            "consider removing the `..s`");
  XLS_ASSERT_OK(PrintPositionalError(
      tm.warnings.warnings().at(0).span, tm.warnings.warnings().at(0).message,
      std::cerr,
      [&](std::string_view) -> absl::StatusOr<std::string> { return program; },
      PositionalErrorColor::kWarningColor));
}

TEST(TypecheckTest, LetWithWildcardMatchGivesWarning) {
  const std::string program = R"(
fn f(x: u32) -> u32 {
  let _ = x + x;
  x
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, Typecheck(program));
  ASSERT_THAT(tm.warnings.warnings().size(), 1);
  std::string filename = "fake.x";
  EXPECT_EQ(tm.warnings.warnings().at(0).span,
            Span(Pos(filename, 2, 6), Pos(filename, 2, 7)));
  EXPECT_EQ(tm.warnings.warnings().at(0).message,
            "`let _ = expr;` statement can be simplified to `expr;` -- there "
            "is no need for a `let` binding here");
  XLS_ASSERT_OK(PrintPositionalError(
      tm.warnings.warnings().at(0).span, tm.warnings.warnings().at(0).message,
      std::cerr,
      [&](std::string_view) -> absl::StatusOr<std::string> { return program; },
      PositionalErrorColor::kWarningColor));
}

TEST(TypecheckTest, UselessTrailingNilGivesWarning) {
  const std::string program = R"(
fn f() -> () {
  trace_fmt!("oh no");
  ()
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, Typecheck(program));
  ASSERT_THAT(tm.warnings.warnings().size(), 1);
  std::string filename = "fake.x";
  EXPECT_EQ(tm.warnings.warnings().at(0).span,
            Span(Pos(filename, 3, 2), Pos(filename, 3, 4)));
  EXPECT_EQ(tm.warnings.warnings().at(0).message,
            "Block has a trailing nil (empty) tuple after a semicolon -- this "
            "is implied, please remove it");
  XLS_ASSERT_OK(PrintPositionalError(
      tm.warnings.warnings().at(0).span, tm.warnings.warnings().at(0).message,
      std::cerr,
      [&](std::string_view) -> absl::StatusOr<std::string> { return program; },
      PositionalErrorColor::kWarningColor));
}

TEST(TypecheckTest, CatchesBadInvocationCallee) {
  constexpr std::string_view kImported = R"(
pub fn some_function() -> u32 { u32:0 }
)";
  constexpr std::string_view kProgram = R"(
import imported

fn main() -> u32 {
  imported.some_function()
})";
  auto import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule module,
      ParseAndTypecheck(kImported, "imported.x", "imported", &import_data));
  EXPECT_THAT(
      ParseAndTypecheck(kProgram, "fake_main_path.x", "main", &import_data),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("An invocation callee must be either a name reference "
                         "or a colon reference")));
}

TEST(TypecheckTest, MissingWideningCastFromValueError) {
  constexpr std::string_view kProgram = R"(
fn main(x: u32) -> u64 {
  widening_cast<u64>()
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid number of arguments passed to")));
}

TEST(TypecheckTest, MissingCheckedCastFromValueError) {
  constexpr std::string_view kProgram = R"(
fn main(x: u32) -> u64 {
  checked_cast<u64>()
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid number of arguments passed to")));
}

TEST(TypecheckTest, MissingWideningCastToTypeError) {
  constexpr std::string_view kProgram = R"(
fn main(x: u32) -> u64 {
  widening_cast(x)
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid number of parametrics passed to")));
}

TEST(TypecheckTest, MissingCheckedCastToTypeError) {
  constexpr std::string_view kProgram = R"(
fn main(x: u32) -> u64 {
  checked_cast(x)
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid number of parametrics passed to")));
}

TEST(TypecheckTest, WideningCastToSmallerUnError) {
  constexpr std::string_view kProgram = R"(
fn main() {
  widening_cast<u33>(u32:0);
  widening_cast<u32>(u32:0);
  widening_cast<u31>(u32:0);
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type uN[32] (32 bits) to "
                                 "uN[31] (31 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastToSmallerSnError) {
  constexpr std::string_view kProgram = R"(
fn main() {
  widening_cast<s33>(s32:0);
  widening_cast<s32>(s32:0);
  widening_cast<s31>(s32:0);
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type sN[32] (32 bits) to "
                                 "sN[31] (31 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastToUnError) {
  constexpr std::string_view kProgram = R"(
fn main() {
  widening_cast<u4>(u3:0);
  widening_cast<u4>(u4:0);
  widening_cast<u4>(s1:0);
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type sN[1] (1 bits) to "
                                 "uN[4] (4 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastsUnError2) {
  constexpr std::string_view kProgram =
      R"(
fn main(x: u8) -> u32 {
  let x_32 = widening_cast<u32>(x);
  let x_4  = widening_cast<u4>(x_32);
  x_32 + widening_cast<u32>(x_4)
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type uN[32] (32 bits) to "
                                 "uN[4] (4 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastToSnError1) {
  constexpr std::string_view kProgram = R"(
fn main() {
  widening_cast<s4>(u3:0);
  widening_cast<s4>(s4:0);
  widening_cast<s4>(u4:0);
}
)";

  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type uN[4] (4 bits) to "
                                 "sN[4] (4 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastsSnError2) {
  constexpr std::string_view kProgram =
      R"(
fn main(x: s8) -> s32 {
  let x_32 = widening_cast<s32>(x);
  let x_4  = widening_cast<s4>(x_32);
  x_32 + widening_cast<s32>(x_4)
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type sN[32] (32 bits) to "
                                 "sN[4] (4 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastsUnToSnError) {
  constexpr std::string_view kProgram =
      R"(
fn main(x: u8) -> s32 {
  let x_9 = widening_cast<s9>(x);
  let x_8 = widening_cast<s8>(x);
  checked_cast<s32>(x_9) + checked_cast<s32>(x_8)
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type uN[8] (8 bits) to "
                                 "sN[8] (8 bits) with widening_cast")));
}

TEST(TypecheckTest, WideningCastsSnToUnError) {
  constexpr std::string_view kProgram =
      R"(
fn main(x: s8) -> s32 {
  let x_9 = widening_cast<u9>(x);
  checked_cast<s32>(x_9)
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Can not cast from type sN[8] (8 bits) to "
                                 "uN[9] (9 bits) with widening_cast")));
}

TEST(TypecheckTest, OverlargeValue80Bits) {
  constexpr std::string_view kProgram =
      R"(
fn f() {
  let x:sN[0] = sN[80]:0x800000000000000000000;
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Value '0x800000000000000000000' does not fit "
                                 "in the bitwidth of a sN[80] (80)")));
}

TEST(TypecheckTest, NegateTuple) {
  constexpr std::string_view kProgram =
      R"(
fn f() -> (u32, u32) {
  -(u32:42, u32:64)
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Unary operation `-` can only be applied to "
                                 "bits-typed operands")));
}

TEST(TypecheckErrorTest, MatchOnBitsWithEmptyTuplePattern) {
  constexpr std::string_view kProgram =
      R"(
fn f(x: u32) -> u32 {
  match x {
    () => x,
  }
}
)";
  EXPECT_THAT(
      Typecheck(kProgram),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("uN[32] Pattern expected matched-on type to be a tuple")));
}

TEST(TypecheckErrorTest, MatchOnBitsWithIrrefutableTuplePattern) {
  constexpr std::string_view kProgram =
      R"(
fn f(x: u32) -> u32 {
  match x {
    (y) => y,
  }
}
)";
  EXPECT_THAT(
      Typecheck(kProgram),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("uN[32] Pattern expected matched-on type to be a tuple.")));
}

TEST(TypecheckErrorTest, MatchOnTupleWithWrongSizedTuplePattern) {
  constexpr std::string_view kProgram =
      R"(
fn f(x: (u32)) -> u32 {
  match x {
    (x, y) => x,
  }
}
)";
  EXPECT_THAT(Typecheck(kProgram),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("(uN[32]) Pattern wanted 2 tuple elements, "
                                 "matched-on value had 1 element")));
}

TEST(TypecheckTest, UnusedBindingInBodyGivesWarning) {
  const std::string program = R"(
fn f(x: u32) -> u32 {
    let y = x + u32:42;
    x
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, Typecheck(program));
  ASSERT_THAT(tm.warnings.warnings().size(), 1);
  std::string filename = "fake.x";
  EXPECT_EQ(tm.warnings.warnings().at(0).message,
            "Definition of `y` (type `uN[32]`) is not used in function `f`");
}

TEST(TypecheckTest, FiveUnusedBindingsInLetBindingPattern) {
  const std::string program = R"(
fn f(t: (u32, u32, u32, u32, u32)) -> u32 {
    let (a, b, c, d, e) = t;
    t.0
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, Typecheck(program));
  ASSERT_THAT(tm.warnings.warnings().size(), 5);
  std::string filename = "fake.x";
  EXPECT_EQ(tm.warnings.warnings().at(0).message,
            "Definition of `a` (type `uN[32]`) is not used in function `f`");
  EXPECT_EQ(tm.warnings.warnings().at(1).message,
            "Definition of `b` (type `uN[32]`) is not used in function `f`");
  EXPECT_EQ(tm.warnings.warnings().at(2).message,
            "Definition of `c` (type `uN[32]`) is not used in function `f`");
  EXPECT_EQ(tm.warnings.warnings().at(3).message,
            "Definition of `d` (type `uN[32]`) is not used in function `f`");
  EXPECT_EQ(tm.warnings.warnings().at(4).message,
            "Definition of `e` (type `uN[32]`) is not used in function `f`");
}

TEST(TypecheckTest, UnusedMatchBindingInBodyGivesWarning) {
  const std::string program = R"(
fn f(x: u32) -> u32 {
  match x {
    y => x
  }
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm, Typecheck(program));
  ASSERT_THAT(tm.warnings.warnings().size(), 1);
  std::string filename = "fake.x";
  EXPECT_EQ(tm.warnings.warnings().at(0).message,
            "Definition of `y` (type `uN[32]`) is not used in function `f`");
}

TEST(TypecheckTest, ConcatU1U1) {
  XLS_ASSERT_OK(Typecheck("fn f(x: u1, y: u1) -> u2 { x ++ y }"));
}

TEST(TypecheckTest, ConcatU1S1) {
  XLS_ASSERT_OK(Typecheck("fn f(x: u1, y: s1) -> u2 { x ++ y }"));
}

TEST(TypecheckTest, ConcatU2S1) {
  XLS_ASSERT_OK(Typecheck("fn f(x: u2, y: s1) -> u3 { x ++ y }"));
}

TEST(TypecheckTest, ConcatU1Nil) {
  EXPECT_THAT(
      Typecheck("fn f(x: u1, y: ()) -> () { x ++ y }").status(),
      IsPosError("XlsTypeError",
                 HasSubstr("uN[1] vs (): Concatenation requires operand types "
                           "to be either both-arrays or both-bits")));
}

TEST(TypecheckTest, ConcatS1Nil) {
  EXPECT_THAT(
      Typecheck("fn f(x: s1, y: ()) -> () { x ++ y }").status(),
      IsPosError("XlsTypeError",
                 HasSubstr("sN[1] vs (): Concatenation requires operand types "
                           "to be either both-arrays or both-bits")));
}

TEST(TypecheckTest, ConcatNilNil) {
  EXPECT_THAT(
      Typecheck("fn f(x: (), y: ()) -> () { x ++ y }").status(),
      IsPosError("XlsTypeError",
                 HasSubstr("() vs (): Concatenation requires operand types to "
                           "be either both-arrays or both-bits")));
}

TEST(TypecheckTest, ConcatU1ArrayOfOneU8) {
  EXPECT_THAT(
      Typecheck("fn f(x: u1, y: u8[1]) -> () { x ++ y }").status(),
      IsPosError("XlsTypeError",
                 HasSubstr("uN[1] vs uN[8][1]: Attempting to concatenate "
                           "array/non-array values together")));
}

TEST(TypecheckTest, ConcatArrayOfThreeU8ArrayOfOneU8) {
  XLS_ASSERT_OK(Typecheck("fn f(x: u8[3], y: u8[1]) -> u8[4] { x ++ y }"));
}

TEST(TypecheckTest, ConcatNilArrayOfOneU8) {
  EXPECT_THAT(Typecheck("fn f(x: (), y: u8[1]) -> () { x ++ y }").status(),
              IsPosError("XlsTypeError",
                         HasSubstr("() vs uN[8][1]: Attempting to concatenate "
                                   "array/non-array values together")));
}

TEST(TypecheckTest, ParametricStructWithoutAllParametricsBoundInReturnType) {
  EXPECT_THAT(
      Typecheck(R"(
struct Point1D<N: u32> { x: bits[N] }

fn f(x: Point1D) -> Point1D { x }
)")
          .status(),
      IsPosError("TypeInferenceError",
                 HasSubstr("Parametric type being returned from function")));
}

}  // namespace
}  // namespace xls::dslx
