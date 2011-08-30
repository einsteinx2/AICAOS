
#include <kos.h>

#include "../aica_common.h"

/* /!\ Invalid pointers - Do NOT deference them! */
static struct io_channel *io_addr_arm;
static struct io_channel *io_addr;

static mutex_t * io_mutex;
static mutex_t * func_mutex[NB_MAX_FUNCTIONS];
static volatile char sync[NB_MAX_FUNCTIONS];

/* Params:
 * 		out = function name (char *)
 * 		in = function ID (unsigned int *)
 */
static SHARED(__get_sh4_func_id)
{
	return aica_find_id((unsigned int *)out, (char *)in);
}

/* Params:
 * 		in = function ID (unsigned int *)
 */
static SHARED(__arm_call_finished)
{
	sync[*(unsigned int *)in] = 1;
	return 0;
}

int aica_init(char *fn)
{
	size_t i;

	aica_clear_handler_table();
	g2_write_32(0xa0900000, 0);

	/* That function will be used by the remote processor to get IDs
	 * from the names of the functions to call. */
	AICA_SHARE(__get_sh4_func_id, FUNCNAME_MAX_LENGTH, sizeof(unsigned int));

	/* Initialize the mutexes. */
	io_mutex = mutex_create();
	for (i=0; i<NB_MAX_FUNCTIONS; i++) {
		func_mutex[i] = mutex_create();
		sync[i] = 0;
	}

	AICA_SHARE(__arm_call_finished, 0, 0);

	/* TODO: It would be faster to use mmap here, if the driver lies on the romdisk. */
	file_t file = fs_open(fn, O_RDONLY);

	if (file < 0)
	  return -1;

	fs_seek(file, 0, SEEK_END);
	int size = fs_tell(file);
	fs_seek(file, 0, SEEK_SET);

	char* buffer = malloc(size);
	if (fs_read(file, buffer, size) < size) {
		fs_close(file);
		free(buffer);
		return -2;
	}

	/* We load the driver into the shared RAM. */
	spu_disable();
	aica_upload(0x0, buffer, size);
	spu_enable();

	fs_close(file);
	free(buffer);

	/* We wait until the ARM-7 writes at the address 0x1FFFFF the message buffer's address. */
	do {
		g2_fifo_wait();
		io_addr_arm = (struct io_channel *)g2_read_32(0xa09FFFFF);
	} while(!io_addr_arm);
	io_addr = (struct io_channel *)((char *)io_addr_arm + 0xa09FFFFF);

	//	spu_dma_init();
	aica_interrupt_init();
	return 0;
}

void aica_exit(void)
{
	asic_evt_disable(ASIC_EVT_SPU_IRQ, ASIC_IRQ9);
	spu_disable();
	aica_clear_handler_table();
}

int __aica_call(unsigned int id, void *in, void *out, unsigned short prio)
{
	struct function_params fparams;
	struct call_params cparams;

	/* We don't want two calls to the same function
	 * to occur at the same time. */
	mutex_lock(func_mutex[id]);

	/* We retrieve the parameters of the function we want to execute */
	aica_download(&fparams, &io_addr_arm[SH_TO_ARM].fparams[id], sizeof(struct function_params));

	/* We will start transfering the input buffer. */
	if (fparams.in.size > 0)
	  aica_upload(fparams.in.ptr, in, fparams.in.size);

	/* Wait until a new call can be made. */
	mutex_lock(io_mutex);

	while (1) {
		aica_download(&cparams.sync, &io_addr_arm[SH_TO_ARM].cparams.sync, sizeof(cparams.sync));
		if (!cparams.sync) break;

		thd_pass();
	}

	/* Fill the I/O structure with the call parameters, and submit the new call.
	 * That function will return immediately, even if the remote function has yet to be executed. */
	cparams.id = id;
	cparams.prio = prio;
	cparams.wait =  fparams.out.size;
	cparams.sync = 1;
	aica_upload(&io_addr_arm[SH_TO_ARM].cparams, &cparams, sizeof(cparams));
	aica_interrupt();
	mutex_unlock(io_mutex);

	/* If the function outputs something, we wait
	 * until the call completes to transfer the data. */
	if (fparams.out.size > 0) {
		while (!sync[id]) thd_pass();
		sync[id] = 0;
		aica_download(out, fparams.out.ptr, fparams.out.size);
	}

	mutex_unlock(func_mutex[id]);
	return 0;
}

void * aica_arm_fiq_hdl_thd(void *param)
{
	struct call_params *cparams = (struct call_params *) param;
	struct function_params fparams;

	aica_download(&fparams, &io_addr_arm[ARM_TO_SH].fparams[cparams->id], sizeof(fparams));

	/* Download the input data. */
	if (fparams.in.size > 0)
	  aica_download(fparams.in.ptr, cparams->in, fparams.in.size);

	/* Call the function. */
	aica_funcp_t func = aica_get_func_from_id(cparams->id);
	(*func)(fparams.in.ptr, fparams.out.ptr);

	/* Upload the output data. */
	if (fparams.out.size > 0)
	  aica_upload(cparams->out, fparams.out.ptr, fparams.out.size);

	/* Free the call_params structure allocated in aica_arm_fiq_hdl(). */
	free(param);

	return NULL;
}

// TODO
static void acknowledge() { }

static void aica_arm_fiq_hdl(uint32_t code)
{
	kthread_t *thd;
	struct call_params *cparams;
	
	cparams = malloc(sizeof(struct call_params));
	aica_download(cparams, &io_addr_arm[ARM_TO_SH].cparams, sizeof(cparams));

	/* The call data has been read, clear the sync flag and acknowledge. */
	cparams->sync = 0;
	aica_upload(&io_addr_arm[ARM_TO_SH].cparams.sync, &cparams->sync, sizeof(cparams->sync));
	acknowledge();

	thd = thd_create(THD_DEFAULTS, aica_arm_fiq_hdl_thd, cparams);
	thd_set_prio(thd, cparams->prio);
}

void aica_interrupt_init(void)
{
	asic_evt_set_handler(ASIC_EVT_SPU_IRQ, aica_arm_fiq_hdl);
	asic_evt_enable(ASIC_EVT_SPU_IRQ, ASIC_IRQ9);
}

void aica_interrupt(void)
{
	g2_fifo_wait();
	g2_write_32(0xa07028a0, 0x20);
}

void aica_update_fparams_table(unsigned int id, struct function_params *fparams)
{
	aica_upload(&io_addr_arm[ARM_TO_SH].fparams[id], fparams, sizeof(struct function_params));
}

