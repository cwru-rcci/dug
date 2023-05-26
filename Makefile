PACKAGE = dug
RELEASE_FILE = $(PACKAGE)
DEBUG_FILE = $(PACKAGE)_debug
MAN_FILE = $(PACKAGE).1

all:
	gcc -D_GNU_SOURCE -Wall -o $(RELEASE_FILE) -pthread -O3 -march=x86-64 dug.c

debug:
	gcc -D_GNU_SOURCE -Wall -g -o $(DEBUG_FILE) -march=x86-64 -pthread -fsanitize=address -fsanitize=leak -fsanitize=undefined dug.c

clean:
	$(RM) $(RELEASE_FILE) $(DEBUG_FILE)

install:
	cp $(RELEASE_FILE) /usr/bin; cp $(MAN_FILE) /usr/share/man/man1/; chmod 755 /usr/bin/$(RELEASE_FILE); chmod 644 /usr/share/man/man1/$(MAN_FILE)

uninstall:
	$(RM) /usr/bin/$(RELEASE_FILE); $(RM) /usr/share/man/man1/$(MAN_FILE)

