PACKAGE = dug
RELEASE_FILE = $(PACKAGE)

all:
	gcc -D_GNU_SOURCE -o $(RELEASE_FILE) -pthread -O3 dug.c

debug:
	gcc -D_GNU_SOURCE -Wall -g -o dug_debug -pthread -fsanitize=address -fsanitize=leak -fsanitize=undefined dug.c

clean:
	$(RM) dug dug_debug

