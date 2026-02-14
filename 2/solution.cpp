#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

static void
execute_command_line(const struct command_line *line);

static int
process_expr_command(const expr *e, const struct command_line *line);

static int
redirect_output_command_line(const struct command_line *line);

static int
execute_command_child(const command &cmd);

static void
execute_command_line(const struct command_line *line)
{
	/* REPLACE THIS CODE WITH ACTUAL COMMAND EXECUTION */

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

			if (process_expr_command(&e, line) != 0) {
				return;
			}
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

static int
process_expr_command(const expr *e, const struct command_line *line)
{
	assert(e->type == EXPR_TYPE_COMMAND);

	pid_t pid = fork();
    if (pid == 0) {
        if (redirect_output_command_line(line) != 0) {
            return -1;
        }

        if (execute_command_child(*e->cmd) != 0) {
			return -1;
		}

        _exit(0);
    } else if (pid > 0) {
        // Parent process - wait for child
        int status;
        waitpid(pid, &status, 0);
    } else {
        perror("fork");
    }

	return 0;
}

static int
redirect_output_command_line(const struct command_line *line)
{
	if (line->out_type == OUTPUT_TYPE_STDOUT) {
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
		return 1;
	}

	// TODO: check errors. 
	dup2(fd, STDOUT_FILENO);
	close(fd);

	return 0;
}


static int
execute_command_child(const command &cmd)
{
	// TODO: add cd and exit support.

	const char **argv = new const char*[cmd.args.size() + 2];
	argv[0] = cmd.exe.c_str();
	for (size_t i = 0; i < cmd.args.size(); i++) {
		argv[i + 1] = cmd.args[i].c_str();
	}
	argv[cmd.args.size() + 1] = NULL;
	
	int res = execvp(argv[0], (char* const*)argv);

	perror("execvp");
	return res;
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
