#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <list>

// #define DEBUG_CMD // Uncomment for debug.

#define STATUS_CD_FAILED 1
#define STATUS_EXIT_FAILED_TOO_MANY_ARGS 1
#define STATUS_EXIT_FAILED_INVALID_ARG 2
#define STATUS_OUTPUT_FORWARD_FAILED 1
#define STATUS_EXEC_ERROR 1
#define STATUS_INTERNAL_ERROR 255

#ifdef DEBUG_CMD
static void 
printf_debug_verbose_command_line(const struct command_line *line) 
{
	assert(line != NULL);
	printf("================================\n");
	printf("Command line:\n");
	printf("Is background: %d\n", (int)line->is_background);
	printf("Output: ");
	if (line->out_type == OUTPUT_TYPE_STDOUT) {
		printf("stdout\n");
	} else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
		printf("new file - \"%s\"\n", line->out_file.c_str());
	} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
		printf("append file - \"%s\"\n", line->out_file.c_str());
	} else {
		assert(false);
	}
	printf("Expressions:\n");
	for (const expr &e : line->exprs) {
		if (e.type == EXPR_TYPE_COMMAND) {
			printf("\tCommand: %s", e.cmd->exe.c_str());
			printf(" %zu args", e.cmd->args.size());
			for (const std::string& arg : e.cmd->args)
				printf(" %s", arg.c_str());
			printf("\n");
		} else if (e.type == EXPR_TYPE_PIPE) {
			printf("\tPIPE\n");
		} else if (e.type == EXPR_TYPE_AND) {
			printf("\tAND\n");
		} else if (e.type == EXPR_TYPE_OR) {
			printf("\tOR\n");
		} else {
			assert(false);
		}
	}
}
#endif

struct output {
	enum output_type type;
	const char *file;
};

// -- Helpers.
#define is_builtin_command(cmd) (((cmd)->exe == "cd") || ((cmd)->exe == "exit"))
#define is_output_file(output) ((output)->type == OUTPUT_TYPE_FILE_NEW || (output)->type == OUTPUT_TYPE_FILE_APPEND)
#define get_home_directory() getenv("HOME")

static int open_output_file_child(const struct output *out);
static int waitpid_exit_code(pid_t pid);
// --

static void execute_command_child(const command *cmd);
static void execute_command_child_fds(const command *cmd, int stdin_fd, int stdout_fd);
static void execute_command_builtin(const command *cmd, bool is_in_pipe, int *status, bool *need_exit);

static pid_t *spawn_pipeline(const command *const *cmds, size_t num,
	const struct output *out, int *status, bool *need_exit);
static void execute_command_line(const struct command_line *line, int *status, bool *need_exit, pid_t *background_pid);

// ----------------------------------------

static int
open_output_file_child(const struct output *out)
{
	int o_flags = O_WRONLY | O_CREAT;
	if (out->type == OUTPUT_TYPE_FILE_NEW)
		o_flags |= O_TRUNC;
	else if (out->type == OUTPUT_TYPE_FILE_APPEND)
		o_flags |= O_APPEND;
	else
		assert(false);

	int fd = open(out->file, o_flags, 0644);
	if (fd < 0) {
		perror("open");
        return -1;
	}
	return fd;
}

static int 
waitpid_exit_code(pid_t pid) 
{
    int status;
    pid_t result = waitpid(pid, &status, 0);
    
    if (result == -1) {
        perror("waitpid failed");
        return -1;
    }
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } 
    else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        return 128 + sig;
    }
    
    return STATUS_INTERNAL_ERROR;
}

static int
get_eof_fd()
{
	int eof_pipe[2];
	if (pipe(eof_pipe) == -1) {
		perror("pipe");
		return -1;
	}
	close(eof_pipe[1]);
	return eof_pipe[0];
}

static void
execute_command_child(const command *cmd)
{
	assert(cmd != NULL);

	const char **argv = new const char*[cmd->args.size() + 2];
	argv[0] = cmd->exe.c_str();
	for (size_t i = 0; i < cmd->args.size(); i++) {
		argv[i + 1] = cmd->args[i].c_str();
	}
	argv[cmd->args.size() + 1] = NULL;
	
	execvp(argv[0], (char* const*)argv);
	perror("execvp");
	_exit(STATUS_EXEC_ERROR);
}

static void
execute_command_child_fds(const command *cmd, int stdin_fd, int stdout_fd)
{
	assert(cmd != NULL);

	if (stdin_fd != -1) {
		if (dup2(stdin_fd, STDIN_FILENO) == -1) {
			perror("dup2");
			_exit(STATUS_INTERNAL_ERROR);
		}
		close(stdin_fd);
	}

	if (stdout_fd != -1) {
		if (dup2(stdout_fd, STDOUT_FILENO) == -1) {
			perror("dup2");
			_exit(STATUS_INTERNAL_ERROR);
		}
		close(stdout_fd);
	}

	execute_command_child(cmd);
}

static void
execute_command_builtin(const command *cmd, bool is_in_pipe, int *status, bool *need_exit)
{
	int last_status = *status;

	*need_exit = false;
	*status = 0; 
	
	if (cmd->exe == "cd") {
		const char *path;
		if (cmd->args.empty()) {
			path = get_home_directory();
			if (path == NULL) {
				return;
			}
		} else {
			path = cmd->args[0].c_str();
		}
		
		if (chdir(path) != 0) {
			perror("cd");
			*status = STATUS_CD_FAILED;
		}
	} else if (cmd->exe == "exit") {
		if (cmd->args.size() > 1) {
			fprintf(stderr, "exit: too many arguments\n");
			*status = STATUS_EXIT_FAILED_TOO_MANY_ARGS;
			return;
		}

		if (cmd->args.size() == 1) {
			char *endptr;
			*status = strtol(cmd->args[0].c_str(), &endptr, 10);
			if (*endptr != '\0') {
				fprintf(stderr, "exit: numeric argument required\n");
				*status = STATUS_EXIT_FAILED_INVALID_ARG;
			}
		} else {
			*status = last_status;
		}

		if (is_in_pipe) {
			return;
		}

		*need_exit = true;
	} else {
		assert(false);
	}
}

static pid_t *
spawn_pipeline(const command *const *cmds, size_t num,
	const struct output *out, int *status, bool *need_exit)
{
	assert(num > 0);

	pid_t *pids = new pid_t[num];
	memset(pids, -1, num * sizeof(pid_t));

	bool is_pipe = (num > 1);
	int prev_pipe_read = -1;

    bool error_occurred = false;

	for (size_t i = 0; i < num; i++) {
		const command *cmd = cmds[i];

		if (is_builtin_command(cmd)) {
			if (prev_pipe_read != -1) {
				close(prev_pipe_read);
			}

			int eof_fd = get_eof_fd();
			if (eof_fd == -1) {
				error_occurred = true;
			    goto cleanup_out;
			}
			prev_pipe_read = eof_fd;

			execute_command_builtin(cmd, is_pipe, status, need_exit);
			continue;
		}

		bool is_last = (i == num - 1);

		int curr_pipe[2] = {-1, -1};
		if (!is_last) {
			if (pipe(curr_pipe) == -1) {
				perror("pipe");
				error_occurred = true;
			    goto cleanup_out;
			}
		}

		pid_t pid = fork();
		if (pid == 0) {
			// Child process.
			if (!is_last)
				close(curr_pipe[0]);

			int stdout_fd = -1;
            if (is_last && out != NULL && is_output_file(out)) {
                stdout_fd = open_output_file_child(out);
                if (stdout_fd == -1) {
					perror("open_output_file_child");
                    _exit(STATUS_OUTPUT_FORWARD_FAILED);
                }
            } else if (!is_last) {
                stdout_fd = curr_pipe[1];
            }

			execute_command_child_fds(cmd, prev_pipe_read, stdout_fd);
		} else if (pid > 0) {
			pids[i] = pid;
			if (prev_pipe_read != -1)
				close(prev_pipe_read);
			if (!is_last) {
				close(curr_pipe[1]);
				prev_pipe_read = curr_pipe[0];
			}
		} else {
			perror("fork");
			if (!is_last) {
				close(curr_pipe[0]);
				close(curr_pipe[1]);
			}
			error_occurred = true;
			goto cleanup_out;
		}
	}

cleanup_out:
	if (prev_pipe_read != -1)
		close(prev_pipe_read);
    if (error_occurred) {
        delete[] pids;
        return NULL;
    }
	return pids;
}

struct pipeline_res {
	bool need_exit;
	int status;
};
	
static pipeline_res
execute_pipeline(const std::vector<const command *> &cmds, const output *out)
{
	bool need_exit = false;
	int status = 0;

	pid_t *pids = spawn_pipeline(cmds.data(), cmds.size(), out,
		&status, &need_exit);
	if (pids == NULL) {
		status = STATUS_INTERNAL_ERROR;
		return {need_exit, status};
	}

	for (size_t i = 0; i < cmds.size() - 1; i++) {
		if (pids[i] != -1) {
			int s;
			waitpid(pids[i], &s, 0);
		}
	}
	
	pid_t last = pids[cmds.size() - 1];
	if (last != -1) {
		int status_waitpid = waitpid_exit_code(last);
		if (status_waitpid >= 0) {
			status = status_waitpid;
			need_exit = false;
		} else {
			status = STATUS_INTERNAL_ERROR;
		}
	}

	delete[] pids;

	return {need_exit, status};
}

enum pipe_execute_cond {
	ALWAYS,
	PREV_SUCCESS,
	PREV_FAILED,
};

struct pipeline {
	std::vector<const command *> cmds;
	enum pipe_execute_cond cond;
};

static pipeline_res
execute_pipelines(const std::vector<pipeline> &pipelines, const output *out)
{
	bool need_exit = false;
	int status = 0;

	for (size_t i = 0; i < pipelines.size(); i++) {
		const pipeline &p = pipelines[i];

		if (p.cond == PREV_SUCCESS && status != 0) 
			continue;
		if (p.cond == PREV_FAILED && status == 0) 
			continue;

		struct pipeline_res res = execute_pipeline(p.cmds, i == pipelines.size() - 1 ? out : NULL);
		assert(res.status >= 0 && res.status != STATUS_INTERNAL_ERROR);
		status = res.status;
		if (res.need_exit) {
			need_exit = true;
			break;
		}
	}

	return {need_exit, status};
}

static void
execute_command_line(const struct command_line *line, int *status, bool *need_exit, pid_t *background_pid)
{
#ifdef DEBUG_CMD
	printf_debug_verbose_command_line(line);
#endif

	if (line->exprs.empty())
		return;

	std::vector<pipeline> pipelines;
	pipelines.push_back({{}, ALWAYS});

	for (const expr &e : line->exprs) {
		switch (e.type) {
		case EXPR_TYPE_PIPE:
			break;
		case EXPR_TYPE_AND:
			pipelines.push_back({{}, PREV_SUCCESS});
			break;
		case EXPR_TYPE_OR:
			pipelines.push_back({{}, PREV_FAILED});
			break;
		case EXPR_TYPE_COMMAND:
			pipelines.back().cmds.push_back(&e.cmd.value());
			break;
		default:
			assert(false);
		}
	}

	assert(!pipelines.back().cmds.empty());

	const struct output out = {
		.type = line->out_type,
		.file = line->out_file.c_str(),
	};

	if (!line->is_background) {
		struct pipeline_res res = execute_pipelines(pipelines, &out);
		*status = res.status;
		*need_exit = res.need_exit;
		return;
	}

	pid_t pid = fork();
	if (pid == 0) {
		int eof_fd = get_eof_fd();
		if (eof_fd == -1) {
			perror("get_eof_fd");
			_exit(STATUS_INTERNAL_ERROR);
		}
		if (dup2(eof_fd, STDIN_FILENO) == -1) {
			perror("dup2");
			_exit(STATUS_INTERNAL_ERROR);
		}
		close(eof_fd);

        struct pipeline_res res = execute_pipelines(pipelines, &out);
        _exit(res.status);
	} else if (pid > 0) {
        *background_pid = pid;
		*status = 0;
		*need_exit = false;
	} else {
		perror("fork");
		*status = STATUS_INTERNAL_ERROR;
	}
}

static void
background_gc(std::list<pid_t> &pids)
{
	auto it = pids.begin();
	while (it != pids.end()) {
		int st = 0;
		pid_t res = waitpid(*it, &st, WNOHANG);
		if (res != 0) {
			it = pids.erase(it);
			continue;
		}
		it++;
	}
}


int
main(void)
{
	int last_status = 0;
    std::list<pid_t> background_pids;

	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}

			int status = 0;
			bool need_exit = false;
            pid_t background_pid = -1;
			execute_command_line(line, &status, &need_exit, &background_pid);
			delete line;

			last_status = status;
			if (background_pid != -1) {
				background_pids.push_back(background_pid);
			}

			if (need_exit) {
				goto out;
			}

			background_gc(background_pids);
		}
	}

out:
	parser_delete(p);
	background_gc(background_pids);
	return last_status;
}
