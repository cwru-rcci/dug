PACKAGE = dug
RELEASE_FILE = $(PACKAGE)
DEBUG_FILE = $(PACKAGE)_debug

all:
	gcc -D_GNU_SOURCE -Wall -o $(RELEASE_FILE) -pthread -O3 dug.c

debug:
	gcc -D_GNU_SOURCE -Wall -g -o $(DEBUG_FILE) -pthread -fsanitize=address -fsanitize=leak -fsanitize=undefined dug.c

clean:
	$(RM) $(RELEASE_FILE) $(DEBUG_FILE)

