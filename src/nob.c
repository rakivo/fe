// A small subset of <https://github.com/tsoding/musializer/blob/master/nob.h>, just to run commands.

#include "nob.h"

void nob_cmd_render(Nob_Cmd cmd, Nob_String_Builder *render)
{
	for (size_t i = 0; i < cmd.count; ++i) {
		const char *arg = cmd.items[i];
		if (arg == NULL) break;
		if (i > 0) nob_sb_append_cstr(render, " ");
		if (!strchr(arg, ' ')) {
			nob_sb_append_cstr(render, arg);
		} else {
			nob_da_append(render, '\'');
			nob_sb_append_cstr(render, arg);
			nob_da_append(render, '\'');
		}
	}
}

Nob_Proc nob_cmd_run_async(Nob_Cmd cmd, bool echo)
{
	if (cmd.count < 1) {
		nob_log(NOB_ERROR, "Could not run empty command");
		return NOB_INVALID_PROC;
	}

	Nob_String_Builder sb = {0};
	if (echo) {
		nob_cmd_render(cmd, &sb);
		nob_sb_append_null(&sb);
		nob_log(NOB_INFO, "CMD: %s", sb.items);
		nob_sb_free(sb);
	}
	memset(&sb, 0, sizeof(sb));
	pid_t cpid = fork();
	if (cpid < 0) {
		nob_log(NOB_ERROR, "Could not fork child process: %s", strerror(errno));
		return NOB_INVALID_PROC;
	}

	if (cpid == 0) {
		// NOTE: This leaks a bit of memory in the child process.
		// But do we actually care? It's a one off leak anyway...
		Nob_Cmd cmd_null = {0};
		nob_da_append_many(&cmd_null, cmd.items, cmd.count);
		nob_cmd_append(&cmd_null, NULL);

		if (execvp(cmd.items[0], (char * const*) cmd_null.items) < 0) {
			nob_log(NOB_ERROR, "Could not exec child process: %s", strerror(errno));
			exit(1);
		}
		NOB_ASSERT(0 && "unreachable");
	}

	return cpid;
}

void nob_log(Nob_Log_Level level, const char *fmt, ...)
{
	switch (level) {
	case NOB_INFO:
		fprintf(stderr, "[INFO] ");
		break;
	case NOB_WARNING:
		fprintf(stderr, "[WARNING] ");
		break;
	case NOB_ERROR:
		fprintf(stderr, "[ERROR] ");
		break;
	default:
		NOB_ASSERT(0 && "unreachable");
	}

	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
}

bool nob_proc_wait(Nob_Proc proc, bool echo)
{
	if (proc == NOB_INVALID_PROC) return false;

	for (;;) {
		int wstatus = 0;
		if (waitpid(proc, &wstatus, 0) < 0) {
			if (echo) nob_log(NOB_ERROR, "could not wait on command (pid %d): %s", proc, strerror(errno));
			return false;
		}

		if (WIFEXITED(wstatus)) {
			int exit_status = WEXITSTATUS(wstatus);
			if (exit_status != 0) {
				if (echo) nob_log(NOB_ERROR, "command exited with exit code %d", exit_status);
				return false;
			}

			break;
		}
	}

	return true;
}

bool nob_cmd_run_sync(Nob_Cmd cmd, bool echo)
{
	Nob_Proc p = nob_cmd_run_async(cmd, echo);
	if (p == NOB_INVALID_PROC) return false;
	return nob_proc_wait(p, echo);
}
