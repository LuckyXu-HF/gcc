/* { dg-do compile } */
/* Verify that atomic op mappings match the Ztso suggested mapping.  */
/* { dg-options "-march=rv64id_ztso -O3" } */
/* { dg-skip-if "" { *-*-* } { "-g" "-flto"} } */
/* { dg-final { check-function-bodies "**" "" } } */

/*
** foo:
**	amoadd\.w\tzero,a1,0\(a0\)
**	ret
*/
void foo (int* bar, int* baz)
{
  __atomic_add_fetch(bar, baz, __ATOMIC_RELEASE);
}
