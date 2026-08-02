// Ділення для об'єктів, зібраних GCCE 4.6.3.
//
// Symbian експортує з drtaeabi лише __aeabi_idivmod/__aeabi_uidivmod - саме їх
// кличе GCC 3.4.3, і саме тому решта проекту лінкується без нарікань. GCC 4.6.3
// для звичайного ділення кличе __aeabi_idiv/__aeabi_uidiv, яких у системі немає
// взагалі. Перехідник віддає ту саму роботу *divmod: за AAPCS частка вже в r0,
// тож зайвої роботи тут не робиться.

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
