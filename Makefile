PACKAGE = dug
RELEASE_FILE = $(PACKAGE)

all:
	gcc -o $(RELEASE_FILE) -pthread -O3 dug.c

clean:
	$(RM) dug

