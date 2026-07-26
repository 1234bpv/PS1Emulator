/* ESTLIB has no strings.h. assem_arm.c:22 includes it for ffs(). */
#ifndef PS1EMU_COMPAT_STRINGS_H
#define PS1EMU_COMPAT_STRINGS_H

/* Позиція наймолодшого встановленого біта, рахуючи з 1; 0 для нуля. */
static __inline__ int ffs(int v)
{
	int i;
	if (v == 0)
		return 0;
	for (i = 1; !(v & 1); i++)
		v = (int)((unsigned int)v >> 1);
	return i;
}

#endif
