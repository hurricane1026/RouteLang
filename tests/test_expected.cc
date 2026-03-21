// Compile: clang++ -std=c++23 -fno-exceptions -fno-rtti -I../include
// test_expected.cpp -o test_expected
#include "core/expected.h"

using core::Expected;
using core::make_unexpected;
using core::Unexpected;

// ── Minimal test harness (no stdlib) ────────────────────────────────

extern "C" void _exit(int);
extern "C" int write(int, const void *, unsigned long);

static int g_total = 0;
static int g_passed = 0;

static void check(bool cond, const char *msg) {
  ++g_total;
  if (!cond) {
    write(2, "FAIL: ", 6);
    int len = 0;
    while (msg[len])
      ++len;
    write(2, msg, len);
    write(2, "\n", 1);
    _exit(1);
  }
  ++g_passed;
}

// ── Helper: int to decimal string (no snprintf) ────────────────────

static void write_int(int fd, int v) {
  char buf[16];
  int i = 15;
  buf[i--] = '\n';
  if (v == 0) {
    buf[i--] = '0';
  } else {
    while (v > 0) {
      buf[i--] = '0' + (v % 10);
      v /= 10;
    }
  }
  ++i;
  write(fd, buf + i, 16 - i);
}

// ── Error type ──────────────────────────────────────────────────────

enum class Err { BadInput, Overflow, NotFound, Timeout };

// ── Non-trivial type (tracks construction/destruction) ──────────────

static int g_ctor_count = 0;
static int g_dtor_count = 0;
static int g_copy_count = 0;
static int g_move_count = 0;

struct Tracked {
  int val;
  Tracked(int v) : val(v) { ++g_ctor_count; }
  Tracked(const Tracked &o) : val(o.val) { ++g_copy_count; }
  Tracked(Tracked &&o) : val(o.val) {
    o.val = -1;
    ++g_move_count;
  }
  ~Tracked() { ++g_dtor_count; }
};

static void reset_counters() {
  g_ctor_count = g_dtor_count = g_copy_count = g_move_count = 0;
}

// ── Non-trivial error type ──────────────────────────────────────────

struct ErrInfo {
  Err code;
  int detail;
  ErrInfo(Err c, int d) : code(c), detail(d) {}
};

// ── Test helper functions ───────────────────────────────────────────

static Expected<int, Err> parse(const char *s) {
  if (s == nullptr)
    return Unexpected(Err::BadInput);
  int len = 0;
  while (s[len])
    ++len;
  return len;
}

static Expected<int, Err> checked_double(int v) {
  if (v > 1000)
    return Unexpected(Err::Overflow);
  return v * 2;
}

static Expected<int, Err> checked_add_ten(int v) {
  if (v > 2000)
    return Unexpected(Err::Overflow);
  return v + 10;
}

// ═══════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════

// ── 1. Basic construction and observers ─────────────────────────────

static void test_basic_value() {
  Expected<int, Err> r = 42;
  check(r.has_value(), "basic value has_value");
  check(static_cast<bool>(r), "basic value bool");
  check(r.value() == 42, "basic value == 42");
  check(*r == 42, "basic deref == 42");
}

static void test_basic_error() {
  Expected<int, Err> r = Unexpected(Err::NotFound);
  check(!r.has_value(), "basic error !has_value");
  check(!static_cast<bool>(r), "basic error !bool");
  check(r.error() == Err::NotFound, "basic error == NotFound");
}

// ── 2. Copy and move ────────────────────────────────────────────────

static void test_copy_value() {
  Expected<int, Err> a = 10;
  Expected<int, Err> b = a; // copy
  check(b.has_value(), "copy value has_value");
  check(b.value() == 10, "copy value == 10");
  check(a.value() == 10, "original unchanged");
}

static void test_copy_error() {
  Expected<int, Err> a = Unexpected(Err::Timeout);
  Expected<int, Err> b = a; // copy
  check(!b.has_value(), "copy error !has_value");
  check(b.error() == Err::Timeout, "copy error == Timeout");
}

static void test_move_value() {
  reset_counters();
  {
    Expected<Tracked, Err> a = Tracked(7);
    Expected<Tracked, Err> b = static_cast<Expected<Tracked, Err> &&>(a);
    check(b.has_value(), "move value has_value");
    check(b.value().val == 7, "move value == 7");
    check(a.value().val == -1, "moved-from value == -1");
  }
  check(g_dtor_count > 0, "move value: dtors called");
}

static void test_move_error() {
  Expected<int, ErrInfo> a = Unexpected(ErrInfo{Err::Overflow, 999});
  Expected<int, ErrInfo> b = static_cast<Expected<int, ErrInfo> &&>(a);
  check(!b.has_value(), "move error !has_value");
  check(b.error().code == Err::Overflow, "move error code");
  check(b.error().detail == 999, "move error detail");
}

// ── 3. Assignment ───────────────────────────────────────────────────

static void test_assign_value_to_value() {
  Expected<int, Err> a = 10;
  Expected<int, Err> b = 20;
  a = b;
  check(a.value() == 20, "assign v->v == 20");
}

static void test_assign_error_to_value() {
  Expected<int, Err> a = 10;
  Expected<int, Err> b = Unexpected(Err::NotFound);
  a = b;
  check(!a.has_value(), "assign e->v !has_value");
  check(a.error() == Err::NotFound, "assign e->v error");
}

static void test_assign_value_to_error() {
  Expected<int, Err> a = Unexpected(Err::BadInput);
  Expected<int, Err> b = 99;
  a = b;
  check(a.has_value(), "assign v->e has_value");
  check(a.value() == 99, "assign v->e == 99");
}

static void test_assign_error_to_error() {
  Expected<int, Err> a = Unexpected(Err::BadInput);
  Expected<int, Err> b = Unexpected(Err::Timeout);
  a = b;
  check(!a.has_value(), "assign e->e !has_value");
  check(a.error() == Err::Timeout, "assign e->e == Timeout");
}

static void test_self_assign() {
  Expected<int, Err> a = 42;
  a = a;
  check(a.has_value(), "self assign has_value");
  check(a.value() == 42, "self assign == 42");
}

static void test_move_assign() {
  Expected<int, Err> a = 10;
  Expected<int, Err> b = 20;
  a = static_cast<Expected<int, Err> &&>(b);
  check(a.value() == 20, "move assign == 20");
}

// ── 4. value_or ─────────────────────────────────────────────────────

static void test_value_or() {
  Expected<int, Err> v = 5;
  Expected<int, Err> e = Unexpected(Err::BadInput);
  check(v.value_or(42) == 5, "value_or pass-through");
  check(e.value_or(42) == 42, "value_or fallback");
  check(e.value_or(0) == 0, "value_or fallback 0");
}

// ── 5. operator-> ───────────────────────────────────────────────────

struct Point {
  int x;
  int y;
};

static void test_arrow_operator() {
  Expected<Point, Err> r = Point{3, 7};
  check(r->x == 3, "arrow x == 3");
  check(r->y == 7, "arrow y == 7");
  r->x = 99;
  check(r->x == 99, "arrow mutation x == 99");
}

// ── 6. TRY macro ────────────────────────────────────────────────────

static Expected<int, Err> try_pipeline(const char *s) {
  auto len = TRY(parse(s));
  auto doubled = TRY(checked_double(len));
  return doubled + 1;
}

static void test_try_success() {
  auto r = try_pipeline("hi");
  check(r.has_value(), "TRY success has_value");
  check(r.value() == 5, "TRY success == 5"); // len=2, *2=4, +1=5
}

static void test_try_first_error() {
  auto r = try_pipeline(nullptr);
  check(!r.has_value(), "TRY first error");
  check(r.error() == Err::BadInput, "TRY first error == BadInput");
}

static void test_try_second_error() {
  // Build a string > 1000 chars so checked_double overflows
  char big[1024];
  for (int i = 0; i < 1023; ++i)
    big[i] = 'x';
  big[1023] = '\0';
  auto r = try_pipeline(big);
  check(!r.has_value(), "TRY second error");
  check(r.error() == Err::Overflow, "TRY second error == Overflow");
}

// TRY with three chained calls
static Expected<int, Err> try_three_step(const char *s) {
  auto a = TRY(parse(s));
  auto b = TRY(checked_double(a));
  auto c = TRY(checked_add_ten(b));
  return c;
}

static void test_try_three_steps() {
  auto r = try_three_step("abc"); // len=3, *2=6, +10=16
  check(r.has_value(), "TRY 3-step has_value");
  check(r.value() == 16, "TRY 3-step == 16");
}

// TRY with non-trivial error type
static Expected<int, ErrInfo> parse_detailed(const char *s) {
  if (s == nullptr)
    return Unexpected(ErrInfo{Err::BadInput, 42});
  int len = 0;
  while (s[len])
    ++len;
  return len;
}

static Expected<int, ErrInfo> try_detailed(const char *s) {
  auto v = TRY(parse_detailed(s));
  return v * 3;
}

static void test_try_non_trivial_error() {
  auto r = try_detailed(nullptr);
  check(!r.has_value(), "TRY non-trivial error");
  check(r.error().code == Err::BadInput, "TRY non-trivial code");
  check(r.error().detail == 42, "TRY non-trivial detail");
}

// TRY with Expected<void, E>
static Expected<void, Err> validate(int v) {
  if (v < 0)
    return Unexpected(Err::BadInput);
  if (v > 100)
    return Unexpected(Err::Overflow);
  return {};
}

static Expected<int, Err> try_with_void(int v) {
  TRY_VOID(validate(v));
  return v * 2;
}

static void test_try_void() {
  auto r1 = try_with_void(10);
  check(r1.has_value(), "TRY void success");
  check(r1.value() == 20, "TRY void == 20");

  auto r2 = try_with_void(-5);
  check(!r2.has_value(), "TRY void error neg");
  check(r2.error() == Err::BadInput, "TRY void BadInput");

  auto r3 = try_with_void(200);
  check(!r3.has_value(), "TRY void error big");
  check(r3.error() == Err::Overflow, "TRY void Overflow");
}

// ── 7. Monadic: and_then ────────────────────────────────────────────

static void test_and_then_success() {
  auto r = parse("abc").and_then(checked_double);
  check(r.has_value(), "and_then success");
  check(r.value() == 6, "and_then == 6");
}

static void test_and_then_first_error() {
  auto r = parse(nullptr).and_then(checked_double);
  check(!r.has_value(), "and_then short-circuit");
  check(r.error() == Err::BadInput, "and_then keeps error");
}

static void test_and_then_second_error() {
  char big[1024];
  for (int i = 0; i < 1023; ++i)
    big[i] = 'x';
  big[1023] = '\0';
  auto r = parse(big).and_then(checked_double);
  check(!r.has_value(), "and_then second error");
  check(r.error() == Err::Overflow, "and_then second Overflow");
}

static void test_and_then_chain() {
  auto r = parse("ab").and_then(checked_double).and_then(checked_add_ten);
  check(r.has_value(), "and_then chain");
  check(r.value() == 14, "and_then chain == 14"); // len=2, *2=4, +10=14
}

static void test_and_then_lambda() {
  auto r = parse("test").and_then([](int v) -> Expected<int, Err> {
    if (v == 0)
      return Unexpected(Err::BadInput);
    return v * v;
  });
  check(r.has_value(), "and_then lambda");
  check(r.value() == 16, "and_then lambda == 16");
}

// ── 8. Monadic: transform ───────────────────────────────────────────

static void test_transform_success() {
  auto r = parse("hi").transform([](int v) { return v * 10; });
  check(r.has_value(), "transform success");
  check(r.value() == 20, "transform == 20");
}

static void test_transform_error() {
  auto r = parse(nullptr).transform([](int v) { return v * 10; });
  check(!r.has_value(), "transform propagates error");
  check(r.error() == Err::BadInput, "transform error == BadInput");
}

static void test_transform_type_change() {
  // int -> bool
  auto r = parse("abc").transform([](int v) { return v > 2; });
  check(r.has_value(), "transform type change");
  check(r.value() == true, "transform bool == true");
}

static void test_transform_chain() {
  auto r =
      parse("ab").transform([](int v) { return v + 100; }).transform([](int v) {
        return v * 2;
      });
  check(r.has_value(), "transform chain");
  check(r.value() == 204, "transform chain == 204"); // (2+100)*2
}

// ── 9. Monadic: or_else ─────────────────────────────────────────────

static void test_or_else_with_value() {
  auto r = parse("ok").or_else([](Err) -> Expected<int, Err> { return 0; });
  check(r.has_value(), "or_else pass-through value");
  check(r.value() == 2, "or_else pass-through == 2");
}

static void test_or_else_recover() {
  auto r = parse(nullptr).or_else([](Err) -> Expected<int, Err> { return 0; });
  check(r.has_value(), "or_else recover");
  check(r.value() == 0, "or_else recover == 0");
}

static void test_or_else_remap_error() {
  // Convert Err -> ErrInfo
  auto r = parse(nullptr).or_else([](Err e) -> Expected<int, Err> {
    if (e == Err::BadInput)
      return Unexpected(Err::NotFound); // remap
    return Unexpected(e);
  });
  check(!r.has_value(), "or_else remap error");
  check(r.error() == Err::NotFound, "or_else remap == NotFound");
}

// ── 10. Mixed monadic chains ────────────────────────────────────────

static void test_mixed_chain() {
  auto r = parse("hi")
               .and_then(checked_double)               // 2 -> 4
               .transform([](int v) { return v + 1; }) // 4 -> 5
               .and_then(checked_add_ten);             // 5 -> 15
  check(r.has_value(), "mixed chain has_value");
  check(r.value() == 15, "mixed chain == 15");
}

static void test_mixed_chain_error_midway() {
  char big[1024];
  for (int i = 0; i < 1023; ++i)
    big[i] = 'x';
  big[1023] = '\0';
  auto r = parse(big)
               .and_then(checked_double)               // 1023 > 1000, error
               .transform([](int v) { return v + 1; }) // skipped
               .and_then(checked_add_ten);             // skipped
  check(!r.has_value(), "mixed chain error midway");
  check(r.error() == Err::Overflow, "mixed chain midway == Overflow");
}

// ── 11. Expected<void, E> ───────────────────────────────────────────

static void test_void_success() {
  Expected<void, Err> r = {};
  check(r.has_value(), "void success");
}

static void test_void_error() {
  Expected<void, Err> r = Unexpected(Err::Timeout);
  check(!r.has_value(), "void error");
  check(r.error() == Err::Timeout, "void error == Timeout");
}

static void test_void_copy() {
  Expected<void, Err> a = {};
  Expected<void, Err> b = a;
  check(b.has_value(), "void copy success");

  Expected<void, Err> c = Unexpected(Err::BadInput);
  Expected<void, Err> d = c;
  check(!d.has_value(), "void copy error");
  check(d.error() == Err::BadInput, "void copy error == BadInput");
}

static void test_void_move() {
  Expected<void, ErrInfo> a = Unexpected(ErrInfo{Err::Overflow, 7});
  Expected<void, ErrInfo> b = static_cast<Expected<void, ErrInfo> &&>(a);
  check(!b.has_value(), "void move error");
  check(b.error().detail == 7, "void move detail");
}

static void test_void_assign() {
  Expected<void, Err> a = {};
  Expected<void, Err> b = Unexpected(Err::NotFound);
  a = b;
  check(!a.has_value(), "void assign e->v");
  check(a.error() == Err::NotFound, "void assign error");

  Expected<void, Err> c = {};
  a = c;
  check(a.has_value(), "void assign v->e");
}

// ── 12. Non-trivial types: construction/destruction tracking ────────

static void test_tracked_value_lifecycle() {
  reset_counters();
  {
    Expected<Tracked, Err> r = Tracked(42);
    check(r.has_value(), "tracked value");
    check(r.value().val == 42, "tracked value == 42");
  }
  // 1 ctor (Tracked(42)) + 1 move (into Expected) = ctor=1, move=1, dtor=2
  check(g_ctor_count == 1, "tracked ctor count");
  check(g_move_count == 1, "tracked move count");
  check(g_dtor_count == 2, "tracked dtor count"); // temp + stored
}

static void test_tracked_error_no_value_dtor() {
  reset_counters();
  {
    Expected<Tracked, Err> r = Unexpected(Err::BadInput);
    check(!r.has_value(), "tracked error path");
  }
  // No Tracked constructed or destroyed
  check(g_ctor_count == 0, "tracked error: no ctor");
  check(g_dtor_count == 0, "tracked error: no dtor");
}

static void test_tracked_copy() {
  reset_counters();
  {
    Expected<Tracked, Err> a = Tracked(10);
    Expected<Tracked, Err> b = a; // copy
    check(b.value().val == 10, "tracked copy == 10");
  }
  check(g_copy_count == 1, "tracked copy count");
}

// ── 13. Different T and E sizes / alignments ────────────────────────

struct Big {
  char data[128];
  int tag;
};
struct Small {
  char c;
};

static void test_size_mismatch() {
  Expected<Big, Small> r1 = Big{{}, 99};
  check(r1.has_value(), "big value");
  check(r1->tag == 99, "big tag == 99");

  Expected<Small, Big> r2 = Unexpected(Big{{}, 77});
  check(!r2.has_value(), "big error");
  check(r2.error().tag == 77, "big error tag == 77");
}

struct Aligned {
  alignas(64) int v;
};

static void test_alignment() {
  Expected<Aligned, Err> r = Aligned{42};
  check(r.has_value(), "aligned value");
  check(r.value().v == 42, "aligned == 42");
  // Verify alignment (address should be 64-byte aligned within struct)
  auto addr = reinterpret_cast<unsigned long>(&r.value().v);
  check((addr % 64) == 0, "aligned 64-byte");
}

// ── 14. make_unexpected ─────────────────────────────────────────────

static void test_make_unexpected() {
  auto u = make_unexpected(Err::Timeout);
  Expected<int, Err> r = u;
  check(!r.has_value(), "make_unexpected");
  check(r.error() == Err::Timeout, "make_unexpected == Timeout");
}

static void test_make_unexpected_lvalue() {
  Err e = Err::NotFound;
  auto u = make_unexpected(e); // lvalue — RemoveRef strips &
  Expected<int, Err> r = u;
  check(r.error() == Err::NotFound, "make_unexpected lvalue");
}

// ── 15. Const correctness ───────────────────────────────────────────

static void test_const_access() {
  const Expected<int, Err> v = 42;
  check(v.has_value(), "const has_value");
  check(v.value() == 42, "const value");
  check(*v == 42, "const deref");
  check(v.value_or(0) == 42, "const value_or");

  const Expected<int, Err> e = Unexpected(Err::BadInput);
  check(!e.has_value(), "const error has_value");
  check(e.error() == Err::BadInput, "const error");
}

static void test_const_monadic() {
  const Expected<int, Err> v = 5;
  auto r = v.and_then(checked_double);
  check(r.value() == 10, "const and_then");

  auto r2 = v.transform([](int x) { return x + 1; });
  check(r2.value() == 6, "const transform");

  const Expected<int, Err> e = Unexpected(Err::BadInput);
  auto r3 = e.or_else([](Err) -> Expected<int, Err> { return 0; });
  check(r3.value() == 0, "const or_else");
}

// ═══════════════════════════════════════════════════════════════════

int main() {
  // 1. Basic
  test_basic_value();
  test_basic_error();

  // 2. Copy / Move
  test_copy_value();
  test_copy_error();
  test_move_value();
  test_move_error();

  // 3. Assignment
  test_assign_value_to_value();
  test_assign_error_to_value();
  test_assign_value_to_error();
  test_assign_error_to_error();
  test_self_assign();
  test_move_assign();

  // 4. value_or
  test_value_or();

  // 5. operator->
  test_arrow_operator();

  // 6. TRY macro
  test_try_success();
  test_try_first_error();
  test_try_second_error();
  test_try_three_steps();
  test_try_non_trivial_error();
  test_try_void();

  // 7. and_then
  test_and_then_success();
  test_and_then_first_error();
  test_and_then_second_error();
  test_and_then_chain();
  test_and_then_lambda();

  // 8. transform
  test_transform_success();
  test_transform_error();
  test_transform_type_change();
  test_transform_chain();

  // 9. or_else
  test_or_else_with_value();
  test_or_else_recover();
  test_or_else_remap_error();

  // 10. Mixed chains
  test_mixed_chain();
  test_mixed_chain_error_midway();

  // 11. Expected<void, E>
  test_void_success();
  test_void_error();
  test_void_copy();
  test_void_move();
  test_void_assign();

  // 12. Non-trivial lifecycle
  test_tracked_value_lifecycle();
  test_tracked_error_no_value_dtor();
  test_tracked_copy();

  // 13. Size / alignment
  test_size_mismatch();
  test_alignment();

  // 14. make_unexpected
  test_make_unexpected();
  test_make_unexpected_lvalue();

  // 15. Const correctness
  test_const_access();
  test_const_monadic();

  // Summary
  write(1, "ALL PASSED: ", 12);
  write_int(1, g_passed);

  return 0;
}
