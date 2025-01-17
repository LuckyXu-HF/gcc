/* { dg-do compile } */
/* Verify that load mappings match the Ztso suggested mapping.  */
/* { dg-options "-march=rv64id_ztso -O3" } */
/* { dg-skip-if "" { *-*-* } { "-g" "-flto"} } */
/* { dg-final { check-function-bodies "**" "" } } */

/*
** foo:
**	fence\trw,rw
**	lw\ta[0-9]+,0\(a0\)
**	sw\ta[0-9]+,0\(a1\)
**	ret
*/
void foo (int* bar, int* baz)
{
  __atomic_load(bar, baz, __ATOMIC_SEQ_CST);
}
