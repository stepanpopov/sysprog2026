#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

// #define DEBUG_CMD // Uncomment for debug.

#define ALWAYS_INLINE inline __attribute__((always_inline))

#define STATUS_CD_FAILED 1
#define STATUS_EXIT_FAILED_TOO_MANY_ARGS 1
#define STATUS_EXIT_FAILED_INVALID_ARG 2
#define STATUS_OUTPUT_FORWARD_FAILED 1
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
}

static void
execute_command_child_fds(const command *cmd, int stdin_fd, int stdout_fd)
{
	assert(cmd != NULL);

	// Child process - setup stdin
	if (stdin_fd != -1) {
		if (dup2(stdin_fd, STDIN_FILENO) == -1) {
			perror("dup2");
			_exit(1);
		}
		close(stdin_fd);
	}

	// Child process - setup stdout
	if (stdout_fd != -1) {
		if (dup2(stdout_fd, STDOUT_FILENO) == -1) {
			perror("dup2");
			_exit(1);
		}
		close(stdout_fd);
	}

	execute_command_child(cmd);
	_exit(1);
}

static ALWAYS_INLINE bool
is_builtin_command(const command *cmd)
{	
	return (cmd->exe == "cd" || cmd->exe == "exit");
}

static ALWAYS_INLINE char*
get_home_directory()
{
	return getenv("HOME");
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
		}
		
		path = cmd->args[0].c_str();
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
    
    return 255;
}

// TODO: replace with cmd.

static int
process_command(const command *cmd, int out_fd, int *status, bool *need_exit)
{
    assert(status != NULL);
    assert(need_exit != NULL);

    if (is_builtin_command(cmd)) {
        execute_command_builtin(cmd, false, status, need_exit);
        return 0;
    }
    *need_exit = false;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        execute_command_child_fds(cmd, -1, out_fd);
    }
    assert(pid > 0);

    int status_res = waitpid_exit_code(pid);
    if (status_res < 0) {
        return -1;
    }
    *status = status_res;

    return 0;
}

static int 
process_commands_pipe(const command **cmds, size_t num, int out_fd, int *status, bool *need_exit)
{
    assert(num > 0);
    
    pid_t *pids = new pid_t[num];
    memset(pids, -1, num * sizeof(pid_t));

    pid_t last_pid = -1;
    int prev_pipe_read = -1;
    int result = 0;
    
    for (size_t i = 0; i < num; i++) {
        const command *cmd = cmds[i];

        if (is_builtin_command(cmd)) {
            if (prev_pipe_read != -1) {
                close(prev_pipe_read);
                prev_pipe_read = -1;
            }

            execute_command_builtin(cmd, true, status, need_exit);
            continue;
        }

        bool is_last = (i == num - 1);
        
        int curr_pipe[2];
        if (!is_last) {
            if (pipe(curr_pipe) == -1) {
                perror("pipe");
                result = -1;
                goto cleanup;
            }
        }

        pid_t pid = fork();
        if (pid == 0) {
            if (!is_last) {
                close(curr_pipe[0]);
            }

            int stdout_fd = is_last ? out_fd : curr_pipe[1];
    
            execute_command_child_fds(cmd, prev_pipe_read, stdout_fd);
        } else if (pid > 0) {
            pids[i] = pid;
            
            if (prev_pipe_read != -1) {
                close(prev_pipe_read);
            }
            
            if (!is_last) {
                close(curr_pipe[1]);
                prev_pipe_read = curr_pipe[0];
            }
        } else {
            result = -1;
            if (!is_last) {
                close(curr_pipe[0]);
                close(curr_pipe[1]);
            }
            goto cleanup;
        }
    }
    
    for (size_t i = 0; i < num - 1; i++) {
        if (pids[i] == -1)
            continue;

        int status;
        waitpid(pids[i], &status, 0);
    }

    last_pid = pids[num - 1];
    if (last_pid != -1) {
        int status_res = waitpid_exit_code(last_pid);
        if (status_res < 0) {
            result = -1;
            goto cleanup;
        }
        *status = status_res;
        *need_exit = false;
    }

cleanup:
    if (prev_pipe_read != -1) {
        close(prev_pipe_read);
    }
    delete[] pids;
    return result;
}

// Returns -1 if open file failed, 0 in other case.
static int
get_out_fd_command_line(const struct command_line *line, int *out_fd)
{
	if (line->out_type == OUTPUT_TYPE_STDOUT) {
		*out_fd = -1;
		return 0;
	}

	int o_flags = O_WRONLY | O_CREAT;
	if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
		o_flags |= O_TRUNC;
	} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
		o_flags |= O_APPEND;
	} else {
		assert(false);
	}

	int fd = open(line->out_file.c_str(), o_flags, 0644);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	*out_fd = fd;
	return 0;
}

static void
execute_command_line(const struct command_line *line, int *status, bool *need_exit)
{
#ifdef DEBUG_CMD
    printf_debug_verbose_command_line(line);
#endif

    if (line->exprs.empty()) {
        return;
	}

	*need_exit = false;
	
	int out_fd;
	if (get_out_fd_command_line(line, &out_fd) != 0) {
		*status = STATUS_OUTPUT_FORWARD_FAILED;
		return;
	}

	if (line->exprs.size() == 1) {
		assert(line->exprs.front().type == EXPR_TYPE_COMMAND);
		
		const command *cmd = &line->exprs.front().cmd.value();
		if (process_command(cmd, out_fd, status, need_exit) != 0) {
			*status = STATUS_INTERNAL_ERROR;
		}
	} else {
		std::vector<const command*> piped_cmds;
		for (const expr &e : line->exprs) {
			switch (e.type) {
			case EXPR_TYPE_PIPE:
				break;
			case EXPR_TYPE_AND:
			case EXPR_TYPE_OR:
				assert(false); // Unsupported AND/OR expression.
				break;
			case EXPR_TYPE_COMMAND:
				piped_cmds.push_back(&e.cmd.value());
				break;
			default:
				assert(false); // Unsupported expression type.
			}
		}
		if (process_commands_pipe(piped_cmds.data(), piped_cmds.size(), out_fd, status, need_exit) != 0) {
			*status = STATUS_INTERNAL_ERROR;
		}
	}
	close(out_fd);
}


int
main(void)
{
	int last_status = 0;

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
			execute_command_line(line, &status, &need_exit);

			last_status = status;
			delete line;
			if (need_exit) {
				goto out;
			}
		}
	}

out:
	parser_delete(p);
	return last_status;
}
