# dug

## Summary
`dug` is a utility similar to `du` that focuses on summarizing usage by group or owner. It is multi-threaded to support parallel walks of the file system, and supports output in JSON format to facilitate use in pipelines and scripts. The utility was developed to untangle quota usage in HPC environments where users belong to many groups that change over time.

## Installing
The utility is intended to run on Linux based systems. A Makefile is included with default target to built the binary `make` and a target to remove the binary `make clean`.

## Usage
```
USAGE: dug [OPTIONS] <directory>

OPTIONS
    -b  Compute apparent size (default is size in bytes of blocks occupied)
    -h  Display help information
    -j  Output result in JSON format (default is plain text)
    -m  Maximum errors before terminating (default is 128)
    -n  Output group/user names (default output uses gids/uids)
    -t  Set number of threads to use (default is 4)
    -u  Summarize usage by owner (default is summarize by group)
    -v  Output information about each file encountered
```

## Limitations
In practice, we have not found the following to be disruptive or frequent, but you should be aware:

* The enumeration does not cross device boundaries.
* Each thread tracks inodes independently. If two directory trees include hard links between descendant files/directories, they will be double counted. E.g., in the following

```
Thread1 Thread2
   |     |
   A     B
  / \   / \
 C   D-E   F
 |   | |   |
...  ...  ...
```
the hard link between descendent directories `D` and `E` causes them to both be traversed by thread 1 and 2. If you find this is causing issues, you can mitigate the problem by running single threaded with `-t 1`.


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
