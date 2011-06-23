
#ifndef _AICA_H
#define _AICA_H

#define SH_TO_ARM 0
#define ARM_TO_SH 1

#define NB_MAX_FUNCTIONS 0x100

#define FUNCNAME_MAX_LENGTH 0x20

typedef int (*aica_funcp_t)(void *, void *);

struct function_flow_params
{
	/* Length in bytes of the buffer. */
	size_t size;

	/* Pointer to the buffer.
	 * /!\ Can be inaccessible from the ARM! */
	void *ptr;
};

struct function_params
{
	/* For each function, two mutexes will be used.
	 * That way, we can transfer the output buffer
	 * of the previous call while starting to
	 * transfer the input buffer for a new call. */
	struct function_flow_params in;
	struct function_flow_params out;
};

/* Contains the parameters relative to one
 * function call. */
struct call_params
{
	/* ID of the function to call */
	unsigned int id;

	/* Priority of the call */
	unsigned short prio;

	/* 0 = Do not wait for the function to complete
	 * (assume there's no output buffer too) */
	unsigned char wait;

	/* Flag value: set when the io_channel is
	 * free again. */
	unsigned char sync;

	/* local addresses where to load/store results */
	void *in;
	void *out;
};

/* That structure defines one channel of communication.
 * Two of them will be instancied: one for the
 * sh4->arm channel, and one for the arm->sh4 channel. */
struct io_channel
{
	/* Params of the function to call. */
	struct call_params cparams;

	/* Array of function params.
	 * fparams[i] contains the params for the
	 * function with the ID=i. */
	struct function_params fparams[NB_MAX_FUNCTIONS];
};

/* Make a function available to the remote processor.
 * /!\ Never use that function directly!
 * Use the macro AICA_SHARE instead. */
void __aica_share(aica_funcp_t func, const char *funcname, size_t sz_in, size_t sz_out);

/* Call a function shared by the remote processor.
 * /!\ Never use that function directly!
 * Use the AICA_ADD_REMOTE macro instead; it will
 * define locally the remote function. */
int __aica_call(unsigned int id, void *in, void *out, unsigned short prio);

/* Called after the remote device requested an ID from a function name. */
int aica_clear_handler(unsigned int id);

/* Clear the whole table. */
void aica_clear_handler_table(void);

/* Update the function params table. */
void aica_update_fparams_table(unsigned int id, struct function_params *fparams);

/* Return the ID associated to a function name. */
int aica_find_id(unsigned int *id, char *funcname);

/* Return the function associated to an ID. */
aica_funcp_t aica_get_func_from_id(unsigned int id);

/* Send data to the remote processor. */
void aica_upload(void *dest, void *from, size_t size);

/* Receive data from the remote processor. */
void aica_download(void *dest, void *from, size_t size);

/* Initialize the interrupt system. */
void aica_interrupt_init(void);

/* Send a notification to the remote processor. */
void aica_interrupt(void);

/* Init the AICA communication subsystem.
 * The parameter is the filename of the driver file
 * (should be NULL when called from the ARM7). */
int aica_init(char *fn);

/* Stop all communication with the AICA subsystem. */
void aica_exit(void);


#define AICA_SHARE(func, sz_in, sz_out) \
  __aica_share(func, #func, sz_in, sz_out)

#define SHARED(func) \
  int func(void *out, void *in)

#define AICA_ADD_REMOTE(func, prio) \
  static unsigned int _##func##_id = -1; \
  int func##(void *out, void *in) \
  { \
	if (_##func##_id < 0) { \
		if (__aica_call(0, #func, &_##func##_id, 0) < 0) { \
			fprintf(stderr, "No ID for function \"%s\".\n", #func); \
			return -1; \
		} \
	} \
	return __aica_call(_##func##_id, in, out, prio); \
  }

#endif

