#ifndef PHP2JAVA_PARSE_H_
#define PHP2JAVA_PARSE_H_

#include "zend_globals.h"
#include "php_globals.h"

PHPAPI int transfer_to_java_file(zend_file_handle *primary_file, int java_version);
PHPAPI int transfer_by_gramar_tree(zend_file_handle *primary_file, int java_version);

#endif