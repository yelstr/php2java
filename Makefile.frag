php2java: $(SAPI_PHP2JAVA_PATH)
	@echo "Install php2java binary"

$(SAPI_PHP2JAVA_PATH): $(PHP_GLOBAL_OBJS) $(PHP_PHP2JAVA_OBJS) $(PHP_BINARY_OBJS)
	$(BUILD_PHP2JAVA)

install-php2java: $(SAPI_PHP2JAVA_PATH)
	@echo "Installing PHP2JAVA binary:        $(INSTALL_ROOT)$(bindir)/"
	@$(mkinstalldirs) $(INSTALL_ROOT)$(bindir)
	@$(INSTALL) -m 0755 $(SAPI_PHP2JAVA_PATH) $(INSTALL_ROOT)$(bindir)/$(program_prefix)php2java$(program_suffix)$(EXEEXT)

clean-php2java:
	@echo "Cleaning php2java object files ..."
	find sapi/php2java/ -name *.lo -o -name *.o | xargs rm -f