# dug

## Summary
`dug` is a utility similar to `du` that focuses on summarizing usage by group or owner. It is multi-threaded to support parallel walks of the file system, and supports output in JSON format to facilitate use in pipelines and scripts. The utility was developed to untangle quota usage in HPC environments where users belong to many groups that change over time. The output describes the total usage under the target directory (broken down by group and a grand total) and the usage by group under each sub-directory of the target.

## Installing
The utility is intended for use on Linux operating systems, and can be built from source or installed via one of the binary releases.

### Binary Install
The releases include RPM and DEB packages, which can be installed using the respective package management commands on RPM and DEB based systems.
Download the appropriate package type for your system from the [releases](https://github.com/cwru-rcci/dug/releases) and install using your system package manager. E.g.:

#### RHEL
```
wget https://github.com/cwru-rcci/dug/releases/download/v1.0.0/dug-1.0.0-1.el8.x86_64.rpm
sudo rpm -i dug-1.0.0-1.el8.x86_64.rpm
```

#### Ubuntu
```
wget https://github.com/cwru-rcci/dug/releases/download/v1.0.0/dug_1.0.0-1_amd64.deb
sudo apt install ./dug_1.0.0-1_amd64.deb
```

### Build from Source Release
The repository includes a Makefile authored to compile using GNU based development tools.
The Makefile includes targets to build the production binary `make`, build a debug binary `make debug`, and remove existing binaries `make clean`.
The Makefile also includes targets to `install` or `uninstall` which copy/remove the binary to/from `/usr/bin` and the man page to/from `/usr/share/man/man1`.

You can download a release of the source code and build it on a Linux system with GCC. An example set of commands using the 1.0.0 source release is:
```
wget https://github.com/cwru-rcci/dug/archive/refs/tags/v1.0.0.tar.gz
tar xzf v1.0.0.tar.gz 
cd dug-1.0.0/
make 
```
This will create a `dug` binary in the source folder that you can run directly, or move/copy to one of the folders on your `$PATH`.

### Build from Repository
You can clone the latest repository code and build on a Linux system with GCC using the following commands:
```
git clone https://github.com/cwru-rcci/dug.git
cd dug
make 
```


## Usage
```
USAGE: dug [OPTIONS] <directory>

OPTIONS
    -b        Compute apparent size (default is size of blocks occupied)
    -h        Output human readable sizes (has no effect when used with -j)
    -j        Output result in JSON format (default is plain text)
    -m <int>  Maximum errors before terminating (default is 128)
    -n        Output group/user names (default output uses gids/uids)
    -t <int>  Set number of threads to use (default is 1)
    -u        Summarize usage by owner (default is summarize by group)
    -v        Output information about each file encountered
    -V        Output version information
    -X <path> Do not process <path> or any descendants. Multiple -X can 
              be specified to exclude multiple files. 
--help  Output usage information
```

## Limitations
In practice, we have not found the following to be disruptive or frequent, but you should be aware:

* The enumeration does not cross device boundaries in worker threads because we track only inodes visited, not [device,inode] pairs.
* Each thread tracks inodes independently. If two directory trees include hard links between descendant files, they will be double counted. E.g., in the following

```
   Thread1 Thread2
      |       |
      A       B
     / \     / \
C---D   E---F   G---H
```
Where Thread 1 inventories directory A, Thread 2 inventories directory B, and hard links exist between descendant file pairs [`C`,`D`], [`E`,`F`] and [`G`,`H`]. 
In this case Thread 1 will count [`D`,`E`], and
Thread 2 will count [`F`,`G`], so
both threads count the hard linked file `E`/`F` because each thread is tracking inodes separately. 
You can mitigate the problem by running single threaded with `-t 1` if it is a significant concern.


## Examples

Inventory the user bob's home directory using 4 threads:

```
dug -t 4 /home/bob
```

Inventory the user alice's home directory, converting sizes to human readable, and resolving numeric IDs to names:

```
dug -n -h /home/alice
```

Inventory a full filesystem, skipping /home, and output as JSON:

```
dug -X /home -j /
```

