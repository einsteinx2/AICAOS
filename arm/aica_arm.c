
#include <stdlib.h>
#include <string.h>

#include "../aica_common.h"
#include "crt0.h"

extern struct io_channel *__io_init;
static struct io_channel *io_addr;

static SHARED(get_arm_func_id)
{
	return aica_find_id((unsigned int *)out, (char *)in);
}

int aica_init(char *fn)
{
	/* Discard GCC warnings about unused parameter */
	(void)fn;

	io_addr = (struct io_channel *) calloc(2, sizeof(struct io_channel));

	aica_clear_handler_table();

	/* That function will be used by the remote processor to get IDs
	 * from the names of the functions to call. */
	AICA_SHARE(get_arm_func_id, FUNCNAME_MAX_LENGTH, sizeof(unsigned int));

	aica_interrupt_init();
	int_enable();

	io_addr[ARM_TO_SH].cparams.sync = 0;
	__io_init = io_addr;

	/* We will continue when the SH-4 will decide so. */
	while (*(volatile int *) &__io_init != 0);

	return 0;
}

void aica_exit(void)
{
	aica_clear_handler_table();
	free(io_addr);
}


int __aica_call(unsigned int id, void *in, void *out, unsigned short prio)
{
	/* Wait here if a previous call of the same function is pending. */
	while(((volatile int) io_addr[ARM_TO_SH].fparams[id].call_status) == FUNCTION_CALL_PENDING);

	/* Protect from context changes. */
	int_disable();

	while(*(volatile unsigned char *) &io_addr[ARM_TO_SH].cparams.sync);
	io_addr[ARM_TO_SH].cparams.id = id;
	io_addr[ARM_TO_SH].cparams.prio = prio;
	io_addr[ARM_TO_SH].cparams.in = in;
	io_addr[ARM_TO_SH].cparams.out = out;
	io_addr[ARM_TO_SH].fparams[id].call_status = FUNCTION_CALL_PENDING;

	aica_interrupt();
	int_enable();

	/* If there is data to be sent back, we will wait until the call completes.
	 * /!\: The call will return immediately even if the remote function has yet to be
	 *      called if there is no data to be retrieved! */
	if (io_addr[ARM_TO_SH].fparams[id].out.size > 0)
		while(((volatile int) io_addr[ARM_TO_SH].fparams[id].call_status) == FUNCTION_CALL_PENDING);

	return 0;
}


void aica_interrupt_init(void)
{
	*(unsigned int *)0x008028b4 = 0x20;
}

void aica_interrupt(void)
{
	*(unsigned int *)0x008028b8 = 0x20;
}

void aica_update_fparams_table(unsigned int id, struct function_params *fparams)
{
	memcpy(&io_addr[SH_TO_ARM].fparams[id], fparams, sizeof(struct function_params));
}

