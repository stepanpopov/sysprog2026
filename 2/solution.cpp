#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

static void
execute_command_line(const struct command_line *line);

// static int
// process_expr_command(const expr *e, const struct command_line *line);

// static int
// redirect_output_command_line(const struct command_line *line);

// static int
// execute_command_child(const command &cmd);


// PIPE
// static void pipe_close_read(int pipefd[2]) {
// 	close(pipefd[0]);
// }

// static int pipe_dup_stdin(int pipefd[2]) {
// 	if (dup2(pipefd[0], STDIN_FILENO) == -1) {
//         perror("dup2 failed");
//         return -1;
//     }
// 	close(pipefd[0]);
// }

// static void pipe_close_write(int pipefd[2]) {
// 	close(pipefd[1]);
// }

// static int pipe_dup_stdout(int pipefd[2]) {
// 	if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
//         perror("dup2 failed");
//         return -1;
//     }
// 	close(pipefd[1]);
// }

//


static void 
printf_verbose_command_line(const struct command_line *line) 
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

static void
execute_command(const command *cmd)
{
	// TODO: add cd and exit support.
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
execute_command_fds(const command *cmd, int stdin_fd, int stdout_fd)
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

	execute_command(cmd);
	_exit(1);
}

static bool
is_builtin_command(const command *cmd)
{	
	return (cmd->exe == "cd" || cmd->exe == "exit");
}

static char*
get_home_directory()
{
	return getenv("HOME");
}

static void
execute_command_builtin(const command *cmd, bool is_output_piped)
{	
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
		}
	} else if (cmd->exe == "exit") {
		if (cmd->args.size() > 1) {
			fprintf(stderr, "exit: too many arguments\n");
			return;
		}

		int exit_code = 0;
		if (cmd->args.size() == 1) {
			char *endptr;
			exit_code = strtol(cmd->args[0].c_str(), &endptr, 10) % 256;
			if (*endptr != '\0') {
				fprintf(stderr, "exit: numeric argument required\n");
				exit_code = 2;
			}
		}

		if (is_output_piped) {
			return;
		}

		exit(exit_code);
	} else {
		assert(false);
	}
}

// TODO: replace with cmd.

static int
process_expr_command(const expr *e, int out_fd)
{
	assert(e->type == EXPR_TYPE_COMMAND);

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	} else if (pid == 0) {
    	execute_command_fds(&(*e->cmd), -1, out_fd);
	}
	assert(pid > 0);

	int status;
	waitpid(pid, &status, 0);

	return 0;
}

static int 
process_expr_commands_pipe(const expr **exprs, size_t num, int out_fd)
{
	assert(num > 0);
    
    pid_t *pids = new pid_t[num];
    int prev_pipe_read = -1;
    int result = 0;
    
    for (size_t i = 0; i < num; i++) {
        assert(exprs[i]->type == EXPR_TYPE_COMMAND);
		const command *cmd = &(*exprs[i]->cmd);

		bool is_last = (i == num - 1);
		if (is_builtin_command(cmd)) {
			execute_command_builtin(cmd, !is_last);
			continue;
		}
		
        
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
	
			execute_command_fds(cmd, prev_pipe_read, stdout_fd);
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
    
    // Wait for all children
    for (size_t i = 0; i < num; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }

cleanup:
	if (prev_pipe_read != -1) {
        close(prev_pipe_read);
    }
    delete[] pids;
    return result;
}

// static int
// process_expr_command(const expr *e, const struct command_line *line)
// {
// 	assert(e->type == EXPR_TYPE_COMMAND);

// 	// Handle 'cd' as a special builtin command in the parent process
// 	if (e->cmd->exe == "cd") {
// 		// cd should change the directory of the parent shell
// 		if (e->cmd->args.empty()) {
// 			fprintf(stderr, "cd: missing argument\n");
// 			return -1;
// 		}
		
// 		const char *path = e->cmd->args[0].c_str();
// 		if (chdir(path) != 0) {
// 			perror("cd");
// 			return -1;
// 		}
// 		return 0;
// 	}
// 	//

// 	pid_t pid = fork();
//     if (pid == 0) {
//         if (redirect_output_command_line(line) != 0) {
//             return -1;
//         }

//         if (execute_command_child(*e->cmd) != 0) {
// 			return -1;
// 		}

//         _exit(0);
//     } else if (pid > 0) {
//         // Parent process - wait for child
//         int status;
//         waitpid(pid, &status, 0);
//     } else {
//         perror("fork");
//     }

// 	return 0;
// }

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


// static int
// redirect_output_command_line(const struct command_line *line)
// {
// 	if (line->out_type == OUTPUT_TYPE_STDOUT) {
// 		return 0;
// 	}

// 	int o_flags = O_WRONLY | O_CREAT;
// 	if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
// 		o_flags |= O_TRUNC;
// 	} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
// 		o_flags |= O_APPEND;
// 	} else {
// 		assert(false);
// 	}

// 	int fd = open(line->out_file.c_str(), o_flags, 0644);
// 	if (fd < 0) {
// 		perror("open");
// 		return 1;
// 	}

// 	// TODO: check errors. 
// 	dup2(fd, STDOUT_FILENO);
// 	close(fd);

// 	return 0;
// }

static void
execute_command_line(const struct command_line *line)
{
	// printf_verbose_command_line(line);

	if (line->exprs.empty())
		return;

	
	int out_fd;
	if (get_out_fd_command_line(line, &out_fd) != 0) {
		// TODO: handle error
		return;
	}

	if (line->exprs.size() == 1) {
		assert(line->exprs.front().type == EXPR_TYPE_COMMAND);
		process_expr_command(&line->exprs.front(), out_fd);
	} else {
		std::vector<const expr*> piped_exprs;
		for (const expr &e : line->exprs) {
			switch (e.type) {
			case EXPR_TYPE_PIPE:
				break;
			case EXPR_TYPE_AND:
			case EXPR_TYPE_OR:
				assert(("Unsupported AND/OR expression", false)); // TODO: support AND and OR
				break;
			case EXPR_TYPE_COMMAND:
				piped_exprs.push_back(&e);
				break;
			default:
				assert(("Unsupported expression type", false));
			}
		}
		process_expr_commands_pipe(piped_exprs.data(), piped_exprs.size(), out_fd);
	}
	close(out_fd);
}

int
main(void)
{
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
			execute_command_line(line);
			delete line;
		}
	}
	parser_delete(p);
	return 0;
}
