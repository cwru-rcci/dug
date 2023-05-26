# dug

## Summary
`dug` is a utility similar to `du` that focuses on summarizing usage by group or owner. It is multi-threaded to support parallel walks of the file system, and supports output in JSON format to facilitate use in pipelines and scripts. The utility was developed to untangle quota usage in HPC environments where users belong to many groups that change over time. The resulting report outlines the total usage under the target directory (broken down by group and a grand total) and the usage by group under each sub-directory of the target.

## Installing
The utility is intended for use on Linux operating systems, and the repository includes a Makefile authored to compile using GNU based development tools.
The Makefile includes targets to build the production binary `make`, build a debug binary `make debug`, and remove existing binaries `make clean`.

### Binary Install
The releases include RPM and DEP packages, which can be installed using the respective package management commands on RPM and DEB based systems.
Download the approprite package type for your system from the [releases](https://github.com/cwru-rcci/dug/releases) and install using your system package manager. E.g.:

#### RHEL
```
wget https://github.com/cwru-rcci/dug/releases/download/v1.0.0-rc1/dug-1.0.0-1.el8.x86_64.rpm
sudo rpm -i dug-1.0.0-1.el8.x86_64.rpm
```

#### Ubuntu
```
wget https://github.com/cwru-rcci/dug/releases/download/v1.0.0-rc1/dug_1.0.0-1_amd64.deb
sudo apt install ./dug_1.0.0-1_amd64.deb
```

### Build from Source Release
You can download a release of the source code and build it on a Linux system with GCC. An example set of commands using the 1.0.0-rc1 source release is:
```
wget https://github.com/cwru-rcci/dug/archive/refs/tags/v1.0.0-rc1.tar.gz
tar xzf dug-1.0.0.tar.gz
cd dug
make 
```
This will create a `dug` binary in the source folder that you can run directly, or move/copy to one of the folders on your `$PATH`. The Makefile
includes targets for install/uninstall that will copy/remove the binary to/from `/usr/bin` and the man page to/from `/usr/share/man/man1`.

### Build from Repository
You can clone the latest repository code and build on a Linux system with GCC using the following commands:
```
git clone https://github.com/cwru-rcci/dug.git
cd dug
make 
```
You can install/uninstall using the `install` or `uninstall` targets of the Makefile.


## Usage
```
USAGE: dug [OPTIONS] <directory>

OPTIONS
    -b  Compute apparent size (default is size of blocks occupied)
    -h  Output human readable sizes (has no effect when used with -j)
    -j  Output result in JSON format (default is plain text)
    -m  Maximum errors before terminating (default is 128)
    -n  Output group/user names (default output uses gids/uids)
    -t  Set number of threads to use (default is 4)
    -u  Summarize usage by owner (default is summarize by group)
    -v  Output information about each file encountered
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
Where thread 1 inventories directory A, thread 2 inventories directory B, and a hard links exist between descendant file pairs [`C`,`D`], [`E`,`F`] and [`G`,`H`]. 
Thread 1 will count [`D`,`E`] (skipping `C` because it is hard linked to file `D` already processed by Thread 1) and 
Thread 2 will count [`F`,`G`] (skipping `H` because it is hard linked to file `G` already processed by Thread 2). 
Both threads count the file represented by `E`/`F` towards the usage summary of their respective directories because they each are tracking inodes separately. 
You can mitigate the problem by running single threaded with `-t 1` if it is a significant concern.


## Example
A simple example to summarize by group, run from inside the git repo.

```
./dug -n -j .
{
  "errors": [

  ],
  "subdirs": {
    ".": {
      "foo":557056
    },
    "./.git": {
      "foo":4653056
    }
  },
  "summary": {
    "foo":5210112
  },
  "total":5210112
}
```
