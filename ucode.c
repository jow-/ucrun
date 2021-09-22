/*
 * Copyright (C) 2021 Jo-Philipp Wich <jo@mein.io>
 * Copyright (C) 2021 John Crispin <john@phrozen.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "ucrun.h"

static uc_parse_config_t config = {
	.strict_declarations = true,
	.raw_mode = true,
};

static int
ucode_run(ucrun_ctx_t *ucrun)
{
	int exit_code = 0;

	/* increase the refcount of the function */
	ucv_get(&ucrun->prog->header);

	/* execute compiled program function */
	uc_value_t *last_expression_result = NULL;
	int return_code = uc_vm_execute(&ucrun->vm, ucrun->prog, &last_expression_result);

	/* handle return status */
	switch (return_code) {
	case STATUS_OK:
		exit_code = 0;

		//char *s = ucv_to_string(&ucrun->vm, last_expression_result);
		//printf("Program finished successfully.\n");
		//printf("Function return value is %s\n", s);
		break;

	case STATUS_EXIT:
		exit_code = (int)ucv_int64_get(last_expression_result);

		printf("The invoked program called exit().\n");
		printf("Exit code is %d\n", exit_code);
		break;

	case ERROR_COMPILE:
		exit_code = -1;

		printf("A compilation error occurred while running the program\n");
		break;

	case ERROR_RUNTIME:
		exit_code = -2;

		printf("A runtime error occurred while running the program\n");
		break;
	}

	/* call the garbage collector */
	ucv_gc(&ucrun->vm);

	return exit_code;
}

static uc_function_t*
ucode_load(const char *file) {
	/* create a source buffer from the given input file */
	uc_source_t *src = uc_source_new_file(file);

	/* check if source file could be opened */
	if (!src) {
		fprintf(stderr, "Unable to open source file %s\n", file);
		return NULL;
	}

	/* compile source buffer into function */
	char *syntax_error = NULL;
	uc_function_t *progfunc = uc_compile(&config, src, &syntax_error);

	/* release source buffer */
	uc_source_put(src);

	/* check if compilation failed */
	if (!progfunc)
		fprintf(stderr, "Failed to compile program: %s\n", syntax_error);

	return progfunc;
}

static void
uc_uloop_timeout_free(ucrun_timeout_t *timeout)
{
	ucv_put(timeout->function);
	ucv_put(timeout->priv);
	list_del(&timeout->list);
	free(timeout);
}

static void
uc_uloop_timeout_cb(struct uloop_timeout *t)
{
	ucrun_timeout_t *timeout = container_of(t, ucrun_timeout_t, timeout);
	uc_value_t *retval = NULL;

	/* push the function and private data to the stack */
	uc_vm_stack_push(&timeout->ucrun->vm, ucv_get(timeout->function));
	if (timeout->priv)
		uc_vm_stack_push(&timeout->ucrun->vm, ucv_get(timeout->priv));
	/* invoke function with zero arguments */
	if (uc_vm_call(&timeout->ucrun->vm, false, timeout->priv ? 1 : 0)) {
		/* function raised an exception, bail out */
		goto out;
	}

	retval = uc_vm_stack_pop(&timeout->ucrun->vm);

	/* if the callback returned an integer, restart the timer */
	if (ucv_type(retval) == UC_INTEGER) {
		uloop_timeout_set(&timeout->timeout, ucv_int64_get(retval));
		return;
	}

out:
	/* free the timer context */
	uc_uloop_timeout_free(timeout);
}

static uc_value_t *
uc_uloop_timeout(uc_vm_t *vm, size_t nargs)
{
	ucrun_timeout_t *timeout;

	uc_value_t *function = uc_fn_arg(0);
	uc_value_t *expire = uc_fn_arg(1);
	uc_value_t *priv = uc_fn_arg(2);

	/* check if the call signature is correct */
	if (!ucv_is_callable(function) || ucv_type(expire) != UC_INTEGER)
		return ucv_int64_new(-1);

	/* add the uloop timer */
	timeout = calloc(1, sizeof(*timeout));
	timeout->function = ucv_get(function);
	timeout->timeout.cb = uc_uloop_timeout_cb;
	timeout->ucrun = vm_to_ucrun(vm);
	if (priv)
		timeout->priv = ucv_get(priv);
	uloop_timeout_set(&timeout->timeout, ucv_int64_get(expire));

	/* track the timer in our context */
	list_add(&timeout->list, &vm_to_ucrun(vm)->timeout);

	return ucv_int64_new(0);
}

static void
uc_uloop_process_free(ucrun_process_t *process)
{
	ucv_put(process->function);
	ucv_put(process->priv);
	list_del(&process->list);
	free(process);
}

static void
uc_uloop_process_cb(struct uloop_process *p, int ret)
{
	ucrun_process_t *process = container_of(p, ucrun_process_t, process);
	uc_value_t *retval = NULL;

	/* push the function and private data to the stack */
	uc_vm_stack_push(&process->ucrun->vm, ucv_get(process->function));
	uc_vm_stack_push(&process->ucrun->vm, ucv_int64_new(ret));
	if (process->priv)
		uc_vm_stack_push(&process->ucrun->vm, ucv_get(process->priv));

	/* execute the callback */
	if (!uc_vm_call(&process->ucrun->vm, false, process->priv ? 2 : 1))
		retval = uc_vm_stack_pop(&process->ucrun->vm);
	ucv_put(retval);

	/* free the timer context */
	uc_uloop_process_free(process);
}

static uc_value_t *
uc_uloop_process(uc_vm_t *vm, size_t nargs)
{
	ucrun_process_t *process;
	pid_t pid;

	uc_value_t *function = uc_fn_arg(0);
	uc_value_t *command = uc_fn_arg(1);
	uc_value_t *priv = uc_fn_arg(2);

	/* check if the call signature is correct */
	if (!ucv_is_callable(function) || ucv_type(command) != UC_ARRAY)
		return ucv_int64_new(-1);

	/* fork of the child */
	pid = fork();
	if (pid < 0)
                return ucv_int64_new(-1);

	/* exec into the requested command */
	if (!pid) {
		int argc, arg;
		char **argv;

		uloop_end();

		/* build the argv structure */
		argc = ucv_array_length(command);
		argv = malloc(sizeof(char *) * (argc + 1));
		argv[argc] = NULL;

		for (arg = 0; arg < argc; arg++) {
			uc_value_t *val = ucv_array_get(command, arg);

			argv[arg] = ucv_to_string(vm, val);
		}

		/* and exec the process */
		execvp(argv[0], argv);
		exit(127);
	}

	/* add the uloop process */
	process = calloc(1, sizeof(*process));
	process->function = ucv_get(function);
	process->process.cb = uc_uloop_process_cb;
	process->ucrun = vm_to_ucrun(vm);
	if (priv)
		process->priv = ucv_get(priv);
	process->process.pid = pid;
	uloop_process_add(&process->process);

	/* track the timer in our context */
	list_add(&process->list, &vm_to_ucrun(vm)->process);

	return ucv_int64_new(0);
}

static void
ucode_init_ubus(ucrun_ctx_t *ucrun)
{
	uc_value_t *ubus = ucv_object_get(ucrun->scope, "ubus", NULL);

	if (!ubus)
		return;

	ucrun->ubus = ucv_get(ubus);
	ubus_init(ucrun);
}

static uc_value_t *
uc_ulog(uc_vm_t *vm, size_t nargs, int severity)
{
	uc_value_t *message = uc_fn_arg(0);
	char *string;

	if (!message)
		return ucv_int64_new(-1);

	string = ucv_to_string(vm, message);
	ulog(severity, "%s", string);
	free(string);

	return ucv_int64_new(0);
}

static uc_value_t *
uc_ulog_info(uc_vm_t *vm, size_t nargs)
{
	return uc_ulog(vm, nargs, LOG_INFO);
}

static uc_value_t *
uc_ulog_note(uc_vm_t *vm, size_t nargs)
{
	return uc_ulog(vm, nargs, LOG_NOTICE);
}

static uc_value_t *
uc_ulog_warn(uc_vm_t *vm, size_t nargs)
{
	return uc_ulog(vm, nargs, LOG_WARNING);
}

static uc_value_t *
uc_ulog_err(uc_vm_t *vm, size_t nargs)
{
	return uc_ulog(vm, nargs, LOG_ERR);
}

static void
ucode_init_ulog(ucrun_ctx_t *ucrun)
{
	uc_value_t *ulog = ucv_object_get(ucrun->scope, "ulog", NULL);
	uc_value_t *identity, *channels;
	int flags = 0, channel;

	/* make sure the declartion is complete */
	if (ucv_type(ulog) != UC_OBJECT)
		return;

	identity = ucv_object_get(ulog, "identity", NULL);
	channels = ucv_object_get(ulog, "channels", NULL);

	if (ucv_type(identity) != UC_STRING || ucv_type(channels) != UC_ARRAY)
		return;

	/* figure out which channels were requested */
	for (channel = 0; channel < ucv_array_length(channels); channel++) {
		uc_value_t *val = ucv_array_get(channels, channel);
		char *v;

		if (ucv_type(val) != UC_STRING)
			continue;

		v = ucv_string_get(val);
		if (!strcmp(v, "syslog"))
			flags |= ULOG_SYSLOG;
		if (!strcmp(v, "stdio"))
			flags |= ULOG_STDIO;
	}

	/* open the log */
	ucrun->ulog_identity = strdup(ucv_string_get(identity));
	ulog_open(flags, LOG_DAEMON, ucrun->ulog_identity);
}

int
ucode_init(ucrun_ctx_t *ucrun, int argc, const char **argv)
{
	uc_value_t *ARGV, *start, *retval = NULL;
	int i;

	/* setup the ucrun context */
	INIT_LIST_HEAD(&ucrun->timeout);
	INIT_LIST_HEAD(&ucrun->process);

	/* initialize VM context */
	uc_vm_init(&ucrun->vm, &config);

	/* load our user code */
	ucrun->prog = ucode_load(argv[1]);
	if (!ucrun->prog) {
		uc_vm_free(&ucrun->vm);
		return -1;
	}
	ucrun->scope = uc_vm_scope_get(&ucrun->vm);

	/* load standard library into global VM scope */
	uc_stdlib_load(uc_vm_scope_get(&ucrun->vm));

	/* load native functions into the vm */
	uc_function_register(ucrun->scope, "uloop_timeout", uc_uloop_timeout);
	uc_function_register(ucrun->scope, "uloop_process", uc_uloop_process);
	uc_function_register(ucrun->scope, "ulog_info", uc_ulog_info);
	uc_function_register(ucrun->scope, "ulog_note", uc_ulog_note);
	uc_function_register(ucrun->scope, "ulog_warn", uc_ulog_warn);
	uc_function_register(ucrun->scope, "ulog_err", uc_ulog_err);

	/* add commandline parameters */
	ARGV = ucv_array_new(&ucrun->vm);

	for (i = 2; i < argc; i++ )
		ucv_array_push(ARGV, ucv_string_new(argv[i]));

	ucv_object_add(ucrun->scope, "ARGV", ARGV);

	/* load our user code */
	ucode_run(ucrun);

	/* enable ulog if requested */
	ucode_init_ulog(ucrun);

	/* everything is now setup, start the user code */
	start = ucv_object_get(ucrun->scope, "start", NULL);
	if (!ucv_is_callable(start))
		return -1;

	/* push the start function to the stack */
	uc_vm_stack_push(&ucrun->vm, ucv_get(start));

	/* execute the start function */
	if (!uc_vm_call(&ucrun->vm, false, 0))
		retval = uc_vm_stack_pop(&ucrun->vm);
	ucv_put(retval);

	/* spawn ubus if requested, this needs to happen after start() was called */
	ucode_init_ubus(ucrun);

	return 0;
}

void
ucode_deinit(ucrun_ctx_t *ucrun)
{
	ucrun_timeout_t *timeout, *t;
	ucrun_process_t *process, *p;
	uc_value_t *stop;

	/* tell the user code that we are shutting down */
	stop = ucv_object_get(ucrun->scope, "stop", NULL);
	if (ucv_is_callable(stop)) {
		uc_value_t *retval = NULL;

		/* push the stop function to the stack */
		uc_vm_stack_push(&ucrun->vm, ucv_get(stop));

		/* execute the stop function */
		if (!uc_vm_call(&ucrun->vm, false, 0))
			retval = uc_vm_stack_pop(&ucrun->vm);
		ucv_put(retval);
	}

	/* start by killing all pending timers */
	list_for_each_entry_safe(timeout, t, &ucrun->timeout, list)
		uc_uloop_timeout_free(timeout);

	/* start by killing all pending processes */
	list_for_each_entry_safe(process, p, &ucrun->process, list)
		uc_uloop_process_free(process);

	/* free ulog */
	if (ucrun->ulog_identity)
		free(ucrun->ulog_identity);

	/* disconnect from ubus */
	ubus_deinit(ucrun);

	/* free program function */
	ucv_put(&ucrun->prog->header);

	/* free VM context */
	uc_vm_free(&ucrun->vm);
}
