#include <stdio.h>

#include "php.h"
#include "SAPI.h"
#include "php_getopt.h"
#include "php2java_parse.h"
#include "php_main.h"

static char *script_filename = "";

static int php2java_startup(sapi_module_struct *sapi_module) /* {{{ */
{
	if (php_module_startup(sapi_module, NULL, 0)==FAILURE) {
		return FAILURE;
	}
	return SUCCESS;
}

static const opt_struct OPTIONS[] = {
	{'f', 1, "file"},
	{'h', 0, "help"},
	{'i', 0, "info"},
	{'l', 0, "syntax-check"},
	{'v', 0, "java-version"},
	{'-', 0, NULL} /* end of args */
};

/* {{{ sapi_module_struct cli_sapi_module
 */
static sapi_module_struct php2java_sapi_module = {
	"php2java",						/* name */
	"Command Line Interface",    	/* pretty name */

	php2java_startup,				/* startup */
	php_module_shutdown,	        /* shutdown */

	NULL,							/* activate */
	NULL,			                /* deactivate */

	NULL,		    	            /* unbuffered write */
	NULL,				            /* flush */
	NULL,							/* get uid */
	NULL,							/* getenv */

	php_error,						/* error handler */

	NULL,		                    /* header handler */
	NULL,			                /* send headers handler */
	NULL,			                /* send header handler */

	NULL,				            /* read POST data */
	NULL,                           /* read Cookies */

	NULL,	                        /* register server variables */
	NULL,			                /* Log message */
	NULL,							/* Get request time */
	NULL,							/* Child terminate */

	STANDARD_SAPI_MODULE_PROPERTIES
};
/* }}} */

static void _2java_usage(char *argv0)
{
    char *prog;
    prog = strrchr(argv0, '/');
    if (prog) {
        prog++;
	} else {
        prog = "php2java";
    }

    printf("Usage: %s [options] [-f] <file> [--] [args...]\n"
            "\n"
            "  -h               This help\n"
            "  -v               java version,values in [8,9,10,11,12,13,14,15,16]\n"
    , prog);
}


static int seek_file_begin(zend_file_handle *file_handle, char *script_file, int *lineno)
{/*{{{*/
    *lineno = 1;

    file_handle->type = ZEND_HANDLE_FP;
    file_handle->opened_path = NULL;
    file_handle->free_filename = 0;
    if (!(file_handle->handle.fp = VCWD_FOPEN(script_file, "rb"))) {
        printf("Could not open input file: %s\n", script_file);
        return FAILURE;
    }
    file_handle->filename = script_file;

    return SUCCESS;
}/*}}}*/

/**
 * transfer php file to java file
 * */
static int do_2java_file(int argc, char *argv[])
{/*{{{*/
    zend_file_handle file_handle;
    char *php_optarg = NULL, *orig_optarg = NULL;
	int php_optind = 1, orig_optind = 1;
    char *script_file = NULL;
    const char *param_error = NULL;
    volatile int exit_status = 0;
    int lineno = 0;
    int c;
    int java_version = 8;

    php_optind = orig_optind;
    php_optarg = orig_optarg;

    while ((c = php_getopt(argc, argv, OPTIONS, &php_optarg, &php_optind, 0, 2)) != -1) {
        switch (c)
        {
            case 'f':
                if (script_file) {
                    param_error = "You can use -f only once.\n";
                }
                script_file = php_optarg;
                break;
        }
    }    

    if (param_error) {
        exit_status = 1; 
        PUTS(param_error);
        goto err;
    }

    if (argc > php_optind
        && !script_file
    ) {
        script_file = argv[php_optind];
        php_optind ++;
    }

    if (script_file) {
        if (seek_file_begin(&file_handle, script_file, &lineno) != SUCCESS) {
            goto err;
        }
    } else {
        _2java_usage(argv[0]); 
        goto out;
    }

    if (transfer_to_java_file(&file_handle, java_version) != SUCCESS) {
        printf("syntax error!\n");
        goto out;
    }

out:
    return exit_status;
err:
    goto out;
}/*}}}*/


int main(int argc, char *argv[])
{/*{{{*/

    int exit_status = 0;
    int php_optind = 1;
    char *php_optarg = NULL;
    int c;
    int module_started = 0;
    int sapi_started = 0;

    while ((c = php_getopt(argc, argv, OPTIONS, &php_optarg, &php_optind, 0, 2)) != -1) {
        switch (c) 
        {
            case '?':
            case 'h':
                _2java_usage(argv[0]);
                goto out;
                break;
        }
    }


    /*start sapi and modules*/
    sapi_module_struct *sapi_module = &php2java_sapi_module;
    sapi_startup(sapi_module);
	sapi_started = 1;

    if (sapi_module->startup(sapi_module) == FAILURE) {
        exit_status = 1;
        goto out;
    }
    module_started = 1;

    do_2java_file(argc, argv);

out:
    if (module_started) {
        php_module_shutdown();
	}
	if (sapi_started) {
        sapi_shutdown();
	}

    exit(exit_status);
}/*}}}*/

