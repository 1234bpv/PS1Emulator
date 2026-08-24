// Division for objects built with GCCE 4.6.3.
//
// Symbian exports only __aeabi_idivmod/__aeabi_uidivmod from drtaeabi - these
// are what GCC 3.4.3 calls, which is why the rest of the project links without issues. GCC 4.6.3
// calls __aeabi_idiv/__aeabi_uidiv for standard division, which do not exist in the system
// at all. The wrapper delegates the same work to *divmod: per AAPCS the quotient is already in r0,
// so no extra work is done here.

extern int __aeabi_idivmod(int aNum, int aDen);
extern unsigned int __aeabi_uidivmod(unsigned int aNum, unsigned int aDen);

int __aeabi_idiv(int aNum, int aDen)
	{
	return __aeabi_idivmod(aNum, aDen);
	}

unsigned int __aeabi_uidiv(unsigned int aNum, unsigned int aDen)
	{
	return __aeabi_uidivmod(aNum, aDen);
	}
