// buggy program - causes an illegal software interrupt

#include <inc/lib.h>

void
umain(void)
{
	// this is a fake page fault.  it can only be used if the DPL is 3
	// if the DPL = 0, this causes a general prot fault, not a PF
	//asm volatile("int $14");

	// this is a real page fault.  volatile, so the compiler doesn't remove it
	// this will cause a PF regardless of DPL, since it's a real PF.
	volatile int x = *((int*)0xc0000000);
}

