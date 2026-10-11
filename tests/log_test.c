/*
 * Unit test driver for src/log.c. Invoked by tests/log_test.sh
 * with a case name. Prints to stdout; the shell harness inspects
 * the output.
 */

#include <stdio.h>
#include <string.h>

#include "log.h"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }

    log_init();

    if (strcmp(argv[1], "default") == 0)
    {
        log_debug("dbg_should_not_appear", "value=%d", 0);
        log_info ("info_event",  "hello");
        log_warn ("warn_event",  "code=%d", 42);
        log_error("error_event", "reason=%s", "boom");
    }
    else if (strcmp(argv[1], "debug") == 0)
    {
        log_debug("debug_event", "value=%d", 7);
    }
    else if (strcmp(argv[1], "escaping") == 0)
    {
        log_info("escape_event",
                 "quote=\" backslash=\\ tab=\t newline_not_in_fmt");
    }
    else
    {
        return 2;
    }

    return 0;
}
