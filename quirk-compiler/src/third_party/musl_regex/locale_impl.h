/* Stand-in for musl's internal locale_impl.h (vendored musl regex, used on
 * Windows only): regerror's messages aren't translated. */
#define LCTRANS_CUR(s) (s)
