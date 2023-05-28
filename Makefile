include config.mk

all: config.mk outdir st

config.mk:
	@if ! test -e config.mk; then printf "\033[31;1mERROR:\033[0m you have to run ./configure\n"; exit 1; fi

OBJ = out/args.o \
			out/thunk.o \
			out/verbose.o \
			out/st.o \
			out/strutil.o \
			out/win.o

$(OBJ):
	$(QUIET_CC)$(CC) $(CFLAGS) -c src/$(@F:.o=.c) -o $@

st: $(OBJ)
	$(QUIET_LINK)$(CC) $^ $(LIBS) -o out/$@

outdir:
	@mkdir -p out

install: st
	@mkdir -p $(DESTDIR)$(BIN_DIR)
	@cp -f out/st $(DESTDIR)$(BIN_DIR)/st
	@chmod 755 $(DESTDIR)$(BIN_DIR)/st
	@strip -s $(DESTDIR)$(BIN_DIR)/st
	@mkdir -p $(DESTDIR)$(MAN_DIR)
	@cp -f data/st.1 $(DESTDIR)$(MAN_DIR)/st.1
	@chmod 644 $(DESTDIR)$(MAN_DIR)/st.1
	@gzip -f $(DESTDIR)$(MAN_DIR)/st.1
	@tic -sx data/st.info
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	@rm -f $(DESTDIR)$(BIN_DIR)/st
	@rm -f $(DESTDIR)$(MAN_DIR)/st.1.gz

clean:
	@echo removing xprop output files..
	@rm -f out/*.o
	@rm -f out/st

distclean: clean
	@echo removing config.mk include file
	@rm -f config.mk

.PHONY: all clean distclean install uninstall
