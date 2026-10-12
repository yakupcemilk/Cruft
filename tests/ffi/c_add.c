/* C ABI partner for tests/cases/ffi.cruft.
 * The same signature is what a Rust `extern "C" fn c_add(a: i32, b: i32) -> i32`
 * would export. */
int c_add(int a, int b) {
    return a + b;
}
