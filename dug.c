/* Copyright (c) 2022 Case Western Reserve University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include<stdio.h>
#include<stdbool.h>
#include<stdlib.h>
#include<dirent.h>
#include<limits.h>
#include<errno.h>
#include<string.h>
#include<unistd.h>
#include<grp.h>
#include<pwd.h>
#include<fts.h>
#include<pthread.h>
#include<sys/stat.h>

#define MAXGIDS    128
#define MAXPATHLEN 4096 
#define INODETABLE 16384 

extern errno;

// triggers all threads to stop before completing
volatile bool exit_now = false;

// Output information about each file/directory encountered
bool verbose = false;

// Output information for debugging execution
bool trace = false;

// Output result in json format
bool json = false;

// Resolve GIDs to group names in output 
bool output_names = false;

// Summarize the usage by UID rather than GID
bool summarize_by_user = false;

// Compute size in blocks occupied rather than actual file size
bool size_in_blocks = true;

// Maximum number of errors to encounter before giving up
int max_errors = 128;

// Number of errors encountered so far
int n_errors = 0;

// Number of threads to use
int n_threads = 4;

// Exit status. Volitile because it can be set in any thread.
volatile int exit_status = 0;

// Error strings are stored here to include in output
char **error_strs;

// Mutex to lock error table on insert
pthread_mutex_t error_mutex;

// Struct to hold inode linked-list entry
struct inode_entry {
    long long unsigned int num;
    void* next;
};

// Struct to hold arguments passed to threads
struct tr_args {
    char* path;
    int** n_results;
    unsigned long long **data;
};

/* SYNOPSIS
 *   Convenience routine to parse a command line argument to a positive integer
 *
 * ARGUMENTS
 *   char* arg : The character data to parse to integer
 *
 * RETURNS
 *   int : >=0 on success, -1 on error
 */
int parse_num(char* arg) {
    errno = 0;
    char* end;
    long i = strtol(arg, &end, 10);
    if(arg == end)
        return -1;

    if(errno == ERANGE)
        return -1;

    if(i > INT_MAX)
       return -1;

    return (int)i; 
}

/* SYNOPSIS
 *   Find the index where the argument GID is stored in the GID table. This
 *   operation uses hashing to locate an initial location and then searches
 *   forward incrementally to find the actual position. If the GID is not
 *   found the index where it should be inserted is returned.
 *
 * ARGUMENT
 *   int gid : The GID to locate
 *   unsigned int gids[] : The GID array to search
 *
 * RETURNS
 *   int : >=0 on success, -1 on error
 */
int find_index(int gid, unsigned int gids[]) {
    int index = gid % MAXGIDS;
    int i = index;
  
    do {
        if(gids[i] == gid)
            return i;
        else if(gids[i] == UINT_MAX) {
            gids[i] = gid;
            return i;
        }
        i = (i+1)%MAXGIDS;
    } while(i!=index);
    
    // Table is full and does not contain
    // an entry for the query GID
    return -1;
}


/* SYNOPSIS
 *   Copy the user/group name corresponding to the argument UID/GID to the 
 *   argument buffer. If the UID/GID cannot be mapped to a name, the numeric
 *   UID/GID is copied to the buffer as a string.
 *
 * ARGUMENT
 *   unsigned int id : The UID/GID to map to a name
 *   char* name : Buffer to copy the name to
 *
 * RETURNS
 *   0 on success, 1 if the GID could not be mapped
 */
int get_name(unsigned int id, char* name) {
    if(summarize_by_user) {
        struct passwd *usr = getpwuid(id);
        if(usr == NULL) {
            sprintf(name, "%u", id);
            return 1;
        }
        else {
            sprintf(name, "%s", usr->pw_name);
            return 0;
        }
    }
    else {
        struct group *grp = getgrgid(id); 
        if(grp == NULL) {
            sprintf(name, "%u", id);
            return 1;
        }
        else {
            sprintf(name, "%s", grp->gr_name);
            return 0;
        }
    }
}


/* SYNOPSIS
 *   Adds an inode to the database if it does not exist
 *
 * ARGUMENT
 *   long long unsigned int num : inode number
 *   struct inode_entry* table[] : inode database
 *
 * RETURN
 *   0 if the inode was added, 1 if it already existed
 */
int insert_inode(long long unsigned int num, struct inode_entry* table[]) {
    int index = num % INODETABLE;
    struct inode_entry *entry = table[index];
    
    // Starting new linked list
    if(entry == NULL) {
        entry = malloc(sizeof(struct inode_entry));
        entry->num = num;
        entry->next = NULL;
	if(trace)
            printf("Added new entry for inode %llu\n", num);
	table[index] = entry;
	return 0;
    }

    // Find existing inode or end of linked list
    while(1) {
	if(entry->num == num)
	    return 1;
	else if(entry->next == NULL)
	    break;
	else
	    entry = entry->next;
    }

    // Insert new inode
    entry->next = malloc(sizeof(struct inode_entry));
    ((struct inode_entry *)(entry->next))->num = num;
    ((struct inode_entry *)(entry->next))->next = NULL;
    if(trace)
        printf("Added LL node for inode %llu\n", num);
    return 0;
}


/* SYNOPSIS
 *   Frees the memory allocated to tracking inodes
 *
 * ARGUMENT
 *   struct inode_entry* table[] : inode database
 *
 * RETURN
 *   Always 0
 */
int free_inode_table(struct inode_entry* table[]) {
    int i;
    struct inode_entry *entry, *next;
    for(i=0;i<INODETABLE;i++) {
	entry = NULL;
	next = NULL;
        if(table[i] != NULL)
            entry = table[i];
            
	while(entry != NULL) {
            next = entry->next;
	    free(entry);
	    entry = next;
        }
    }
    return 0;
}

/* SYNOPSIS
 *   Add a new GID to the group database with intial size, or upadate an 
 *   existing entry
 *
 * ARGUMENT
 *   unsigned int gid : GID to insert/update
 *    long long unsigned int size : size for initialization or increment
 *    unsigned int gids[]: GID database
 *    long long unsigned int sizes[]: Size database
 *
 * RETURN
 *   0 on success, 1 if the database is full and the GID cannot be inserted
 */
int insert_or_update(unsigned int gid, long long unsigned int size, unsigned int gids[], long long unsigned int sizes[]) {
    int index = find_index(gid, gids);
    if(index == -1) {
        exit_now = true;
        exit_status = 2;
        return 1;
    }
    sizes[index] += size;
    return 0;
}


/* SYNOPSIS
 *   Store an error message
 *
 * ARGUMENT
 *   char* path : The file/directory path where the error was encountered
 *   char* error : The error message
 *
 * RETURN
 *   0 on success, 1 if the maximum number of errors has been exceeded
 */
int store_error(char* path, char* error) {
    if(n_errors >= max_errors) {
        exit_now = true;
        exit_status = 3;
        return 1;
    }

    // Use mutex to manage concurrent access to global
    // errors array
    pthread_mutex_lock(&error_mutex);

    int total_length = strlen(path)+strlen(error);
    error_strs[n_errors] = malloc(total_length+10);
    sprintf(error_strs[n_errors], "%s: %s", path, error);
    n_errors++;

    pthread_mutex_unlock(&error_mutex);
    return 0;
}


/* SYNOPSIS
 *   Escape/remove special characters so the string is a valid JSON string
 *
 * ARGUMENT
 *   char* path : The initial string
 *   char* escaped : Buffer to hold the escaped string
 *
 * RETURN
 *   Always 0
 */
int json_escape_str(char* path, char* escaped) {
    int len = strlen(path);
    int i,j=0;
    for(i=0;i<len;i++) {
        if(path[i] == '\\') {
            escaped[j] = '\\';
            escaped[j+1] = '\\';
            j+=2; 
        }
        else if(path[i] == '\n' || path[i] == '\r' || path[i] == '\b') {
            escaped[j] = '_';
            j+=1;
        }
        else {
            escaped[j] = path[i];
            j+=1;
        }   
    }
    escaped[j] = '\0';
    return 0;
}


/* SYNOPSIS
 *   If the command failed, outputs a JSON formatted failure
 *
 * ARGUMENT
 *   None
 *
 * RETURN
 *   Always 0
 */
int json_output_failure() {
    int i;

    printf("{\n  \"failure\": true,\n  \"errors\": [\n");
    for(i=0;i<n_errors;i++) {
        if(i > 0)
            printf(",\n");
        printf("    \"%s\"", error_strs[i]);
    }
    printf("\n  ]\n}\n");

    return 0;
}


/* SYNOPSIS
 *   Output result in plain text format
 *
 * ARGUMENT
 *   void* results : Results database
 *   int n_results : Number of results
 *   long long unsigned int total: The total use across all files in result
 *
 * RETURN
 *   Always 0
 */
int output_table(void* results, int n_results, long long unsigned int total) {
    struct tr_args **descendents = results;
    int i, j;
    unsigned long long int gid, size;
    char* name_buffer = malloc(MAXPATHLEN);
    if(n_errors > 0) {
        printf("=================== Errors ===================\n");
        for(i=0;i<n_errors;i++)
            printf("%s\n", error_strs[i]);
        printf("\n\n");
    }
   
    printf("=================== Sub Directories ====================\n"); 
    for(i=0;i<n_results-1;i++) {
        json_escape_str(descendents[i]->path, name_buffer);
        printf("%s\n", name_buffer);
        for(j=0;j<**(descendents[i]->n_results)*2;j+=2) {
            gid = (*(descendents[i]->data))[j];
            size = (*(descendents[i]->data))[j+1];
            if(output_names)
                get_name(gid, name_buffer);
            else
                sprintf(name_buffer, "%llu", gid);
            printf("%24s  %llu\n", name_buffer, size);
        }
        printf("\n");
    }
    printf("\n=================== Summaries ===================\n");
    for(j=0;j<**(descendents[n_results-1]->n_results)*2;j+=2) {
        gid = (*(descendents[n_results-1]->data))[j];
        size = (*(descendents[n_results-1]->data))[j+1];
        if(output_names)
            get_name(gid, name_buffer);
        else
            sprintf(name_buffer, "%llu", gid);
        printf("%24s  %llu\n", name_buffer, size);
    }
    printf("%24s  %llu\n", "Total", total);
    free(name_buffer);
    return 0;
}


/* SYNOPSIS
 *   Outputs the result of the command as a JSON object
 *
 * ARGUMENT
 *   void* results : Results database
 *   int n_results : Number of results
 *   long long unsigned int total: The total use across all files in result
 *
 * RETURN
 *   Always 0
 */
int output_json(void* results, int n_results, long long unsigned int total) {
    struct tr_args **descendents = results;
    int i, j;
    int out_dir = 0, out_size = 0;
    unsigned long long int gid, size;
    char* name_buffer = malloc(MAXPATHLEN);
    printf("{\n  \"errors\": [\n");
    
    for(i=0;i<n_errors;i++) {
        if(i > 0)
            printf(",\n");
        printf("    \"%s\"", error_strs[i]);
    }
    printf("\n  ],\n  \"subdirs\": {\n");
    for(i=0;i<n_results-1;i++) {
        out_size = 0;
        if(out_dir > 0)
            printf(",\n");
        out_dir += 1;

        json_escape_str(descendents[i]->path, name_buffer);
        printf("    \"%s\": {\n", name_buffer);
        for(j=0;j<**(descendents[i]->n_results)*2;j+=2) {
            gid = (*(descendents[i]->data))[j];
            size = (*(descendents[i]->data))[j+1];
            if(output_names)
                get_name(gid, name_buffer);
            else
                sprintf(name_buffer, "%llu", gid);
            if(out_size > 0)
                printf(",\n");
            out_size += 1;
            printf("      \"%s\":%llu", name_buffer, size);
        }
        printf("\n    }");
    }
    printf("\n  },\n");

    // Output the group totals summary
    out_size = 0;
    printf("  \"summary\": {\n");
    for(j=0;j<**(descendents[n_results-1]->n_results)*2;j+=2) {
        gid = (*(descendents[n_results-1]->data))[j];
        size = (*(descendents[n_results-1]->data))[j+1];
        if(output_names)
            get_name(gid, name_buffer);
        else
            sprintf(name_buffer, "%llu", gid);
        if(out_size > 0)
            printf(",\n");
        out_size += 1;
        printf("    \"%s\":%llu", name_buffer, size);
    }
    printf("\n  },\n");

    // Output the grand total 
    printf("  \"total\":%llu", total);
    printf("\n}\n");
    free(name_buffer);
    return 0;
}

/* SYNOPSIS:
 *   Initialize a new empty result
 *
 * ARGUMENT
 *   struct tr_args **result : Pointer to an address where we will initialize the
 *                             result 
 *   char* dir : The file/directory path associated with the result
 *
 * RETURN
 *   Void
 */
void init_result(struct tr_args **result, char* dir) {
    (*result) = (struct tr_args*)malloc(sizeof(struct tr_args));
    (*result)->path = malloc(strlen(dir)+1);
    sprintf((*result)->path, "%s", dir);
    (*result)->n_results = (int**)malloc(sizeof(int**));
    (*result)->data = (long long unsigned int**)malloc(sizeof(long long unsigned int**));
}


/* SYNOPSIS
 *   Free the memory allocated to a result
 *
 * ARGUMENT
 *   struct tr_args **result : Pointer to the address of a result
 *
 * RETURN
 *   Void 
 */
void free_result(struct tr_args **result) {
  free(*((*result)->data));
  free((*result)->data);
  free(*((*result)->n_results));
  free((*result)->n_results);
  free((*result)->path);
  free(*result);
}


/* SYNOPSIS
 *   Copies the database of storage-by-gid into a result structure. The
 *   method interleaves the arrays of gid[] and size[] into a single
 *   array of pairs
 * ARGUMENT
 *   struct tr_args *result : Address of result to populate
 *   unsigned int gids[]: GIDs of groups that were encountered
 *   long long unsigned int sizes[]: Storage usage for each GID encountered
 * RETURN
 *   Void
 */
void pack_result(struct tr_args *result, unsigned int gids[], long long unsigned int sizes[]) {
    int n_groups = 0;
    int i, j;

    // Size of packed result is size of groups encoutered,
    // so we sweep the GID table one time to find how many
    // groups are stored
    for(i=0;i<MAXGIDS;i++) {
        if(gids[i] != UINT_MAX)
            n_groups += 1;
    }
    *(result->n_results) = (int*)malloc(sizeof(int)); 
    *(*(result->n_results)) = n_groups;
    *(result->data) = calloc(n_groups*2, sizeof(long long unsigned int));
    j = 0;
    for(i=0;i<MAXGIDS;i++) {
        if(gids[i] != UINT_MAX) {
            (*(result->data))[j] = gids[i];
            (*(result->data))[j+1] = sizes[i];
            j += 2;
        }
    }
}

/* SYNOPSIS
 *   Iterates over all results generated to compile a summary of total
 *   usage by group
 * ARGUMENT
 *   void* tstructs: A pointer to the array oif results
 *   int n_results: The number of results in the array
 *   long long unsigned int *total: Address where a grand total is stored
 * RETURN
 *   0 on success, 1 on failure
 */
int add_summary(void* tstructs, int n_results, long long unsigned int *total) {
    struct tr_args **results = tstructs;
    int i, j;
    unsigned long long int size, sizes[MAXGIDS];
    unsigned int gid, gids[MAXGIDS];

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    // i<n_results-1 because the results of the target
    // directory and sub-directories are in indices
    // [0,n_results-2] and the summary is stored at
    // index n_results-1
    for(i=0;i<n_results-1;i++) {
        for(j=0;j<**(results[i]->n_results)*2;j+=2) {
            gid = (*(results[i]->data))[j];
            size = (*(results[i]->data))[j+1];
            if(insert_or_update(gid, size, gids, sizes) != 0)
                return 1;
            *total += size;
        }
    }
    
    pack_result(results[n_results-1], gids, sizes);
    return 0;
}


/* SYNOPSIS
 *   Compiles a summary of file usage in a directory and all descendents,
 *   organized by group (GID). The method is designed to be launched as 
 *   a thread.
 * ARGUMENT:
 *  void *arg : Thread argument that stores the path to traverse, along
 *              with pointers to addresses where the result will be
 *              stored when the method completes
 * RETURN
 *   char* status: "OK" on success, and other strings on error
 */
static void* fts_walk(void *arg) {
    FTS *stream;
    FTSENT *entry;
    int i;
    bool insert = false;
    bool error = false;
    long long unsigned int audit_size;
    long long unsigned int sizes[MAXGIDS];
    unsigned int id;
    unsigned int gids[MAXGIDS];
    struct inode_entry *table[INODETABLE];
    struct tr_args *targs = arg;
    char* path = targs->path;

    // FTS needs a null-terminated list of paths as argument
    char *paths[2] = {path, NULL};

    // FTS_PHSYCIAL: do not follow symbolic links
    // FTS_XDEV    : do not descend into directories on devices
    //               that differ from the starting directory
    // FTS_NOCHDIR : do not chdir into each sub-directory. This
    //               allows us use multiple fts threads from 
    //               one process
    stream = fts_open(paths, FTS_PHYSICAL|FTS_XDEV|FTS_NOCHDIR, NULL);
    if(stream == NULL) {
        store_error(paths[0], strerror(errno));
        return "FTSOPENFAIL"; 
    }

    // Fil the group/usage hash table with initialization values
    // We use UINT_MAX for uninitialized GID because root GID is 0
    // We use 0 for uninitialized size so that we can update using +=
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    // Fill the inode lookup table with NULL to indicate empty for all entries 
    for(i=0;i<INODETABLE;i++) {
        table[i] = NULL;
    }

    // Read from the FTS stream until it is empty
    while((entry=fts_read(stream))) {
        // FTS error, entry was null. We store the error and continue,
        // but this increments number of errors and potentially sets
        // exit_now=true if maximum errors is reached
        if(entry == NULL) {
            store_error("fts_error", strerror(errno));
            continue;
        }

        // If maximum errors were encountered, or other unrecoverable
        // errors occured, this indicates to terminate execution
        if(exit_now)
            return "TASKEXIT";

        // Process the file or directory
        insert = false;
        error = false;
        switch(entry->fts_info) {
            // Regular file
            case FTS_F:
                if(verbose)
                    printf("+file      %s (%ld)\n", entry->fts_path, entry->fts_statp->st_size);
		insert = true;
                break;
            // Directory
            case FTS_D:
                if(verbose)
                    printf("+directory %s (%ld)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            // Symbolic link
            case FTS_SL:
                if(verbose)
                    printf("+symlnk    %s (%ld)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            // Broken symlink
            case FTS_SLNONE:
                if(verbose)
                    printf("+brksymlnk %s (%ld)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            // Uncategorized file
            case FTS_DEFAULT:
                if(verbose)
                    printf("+uncat     %s (%ld)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            // A directory we could not descend into
            case FTS_DNR:
                error = true;
                break;
            // A directory we already traversed in pre-order
            case FTS_DP:
                // We already saw this directory in preorder FTS_D,
                // so ignore it
                break;
            // A file we could not stat
            case FTS_NS:
                if(verbose)
                    printf("-stat_err  %s %s\n", entry->fts_path, strerror(errno));
                error = true;
                break;
            // Unclassified error
            case FTS_ERR:
                if(verbose)
                    printf("-fts_err   %s\n", entry->fts_path);
                error = true;
                break;
            // Nothing else matched, so log it as skipped in verbose mode
            default: 
                if(verbose)
                    printf("-fts_skip  %s\n", entry->fts_path);
        }

        // Store error, and exit if maximum errors reached 
        if(error) {
            if(store_error(entry->fts_path, strerror(entry->fts_errno)) != 0) {
                return "MAXERRORS";
            }
        }

        // Skip inodes that have been previously visited
	if(insert && (entry->fts_statp->st_nlink > 1)) {
	    if((i=insert_inode(entry->fts_statp->st_ino, table)) != 0) {
                insert = false;
	        if(trace)
	            printf("-inode   %s inode %lu has already been counted\n", entry->fts_path, entry->fts_statp->st_ino);
	    }
	}

        // Update the running usage in the hash table
        if(insert) {
            // Compute size as either file size, or size of
            // blocks the file spans
            audit_size = entry->fts_statp->st_size;
            if(size_in_blocks) {
                audit_size = entry->fts_statp->st_blocks*512;
            }

            id = entry->fts_statp->st_gid;
            if(summarize_by_user)
                id = entry->fts_statp->st_uid;

            if(insert_or_update(id, audit_size, gids, sizes) != 0) {
                store_error(entry->fts_path, "GID table overflowed");
                return "GID_OVERFLOW";
            }
        }

    }
    pack_result(targs, gids, sizes);
    free_inode_table(table);

    fts_close(stream);
    return "OK";
}

/* SYNOPSIS
 *   Scans an argument directory to determine the number of subdirectories
 *   within it.
 * ARGUMENT
 *   char* path : The directory to scan
 *   unsigned int *n_subdirs : Address where the count is stored
 *   long long unsinged int devnum: Address to store directory device num
 * RETURN
 *   0 on success, 1 on error
 */
int get_n_subdirs(char* path, unsigned int *n_subdirs, long long unsigned int *devnum) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    char* temppath = malloc(MAXPATHLEN);
    unsigned int sd = 0;

    // Open directory for reading or return with error
    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        return 1;
    }

    // stat the directory to get device number
    if(lstat(path, &meta) != 0) {
        store_error(temppath, "Could not stat file");
        free(temppath);
	return 1;
    }
    *devnum = meta.st_dev;

    // Read the directory and count the number of subdirectories
    // on the same device
    while((entry=readdir(dp))) {
        if(strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0)
            continue;

        sprintf(temppath, "%s/%s", path, entry->d_name);
        if(lstat(temppath, &meta) != 0) {
            store_error(temppath, "Could not stat file");
            continue;
        }

	if(((meta.st_mode & S_IFMT) == S_IFDIR) && (meta.st_dev == *devnum))
            sd += 1;
    }
 
    *n_subdirs = sd;
    closedir(dp);
    free(temppath);
    return 0;
}


/* SYNOPSIS
 *   Sweeps an array of thread ids and attempts to join active threads
 * ARGUMENT
 *   pthread_t *thread_ids[] : Thread ids to sweep
 *   unsigned int max_n_threads : Length of thread id array
 * RETURN
 *   -1 if no threads were joined, index of first joined thread otherwise
 */
int tr_recover_slots(pthread_t *thread_ids[], unsigned int max_n_threads) {
    int i, status, n=-1;
    for(i=0;i<max_n_threads;i++) {
        if(thread_ids[i] == NULL)
            continue;

        status = pthread_tryjoin_np(*thread_ids[i], NULL);
        if(status == 0) {
            free(thread_ids[i]);
            thread_ids[i] = NULL;
            if(n == -1)
                n = i;
        }
    }
    return n;
}

/* SYNOPSIS
 *   Find the index of an empty array element where a thread id can be stored.
 *   The method polls until an element becomes available.
 * ARGUMENT
 *   pthread_t *thread_ids[] : Array of thread ids
 *   unsigned int max_n_threads : Length of thread id array
 * RETURN
 *   The index of the available element
 */
int tr_find_slot(pthread_t *thread_ids[], unsigned int max_n_threads) {
    int i;
    while(1) {
        // If a slot is available, return the index
        for(i=0;i<max_n_threads;i++) {
            if(thread_ids[i] == NULL)
                return i;
        }

        // Otherwise, try to recover slots from completed threads.
        // If no slots recovered, wait a short interval. Otherwise, 
        // return the index of the first slot we opened up. 
        if((i=tr_recover_slots(thread_ids, max_n_threads)) == -1) {
            //printf("tr   :Waiting for thread slot to become available\n");
            usleep(10000);
        }
        else
            return i;
    }
}

/* SYNOPSIS
 *   Waits for and joins all threads with ids in the argument array.
 * ARGUMENT
 *   pthread_t *thread_ids[] : Array of thread ids
 *   unsigned int max_n_threads : Length of thread id array
 * RETURN
 *   0 if all join are successful, number of of joins that failed otherwise
 */
int tr_finalize(pthread_t *thread_ids[], unsigned int max_n_threads) {
    int i, status, n=0;
    for(i=0;i<max_n_threads;i++) {
        if(thread_ids[i] == NULL)
            continue;

        status = pthread_join(*thread_ids[i], NULL);
        if(status != 0)
            printf("tr   :Error in pthread_join(): %s\n", strerror(errno));
	n += status;
        free(thread_ids[i]);
    }
    return n;
}

/* SYNOPSIS
 *   Inventories the usage in this directory and all subdirectories, organized
 *   by path and groups (gids).
 * ARGUMENT:
 *   char* path : The path to the top level directory of the search
 *   unsigned int max_n_threads : The number of threads to use for the search
 * RETURN
 *   0 on success, 1 on failure
 */
int walk(char* path, unsigned int max_n_threads) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    int i, status, thread_i;
    char* temppath = malloc(MAXPATHLEN);
    bool insert, process;
    long long unsigned int audit_size, grand_total=0, devnum=0;
    long long unsigned int sizes[MAXGIDS];
    unsigned int gids[MAXGIDS];
    unsigned int id;
    unsigned int n_subdirs = 0, subdir_count=1;
    struct inode_entry *table[INODETABLE];


    // Find the number of sub-directories under the root
    // path
    if((status=get_n_subdirs(path, &n_subdirs, &devnum)) != 0) {
        exit_status = 1;
        return 1;
    }

    // Open the directory to enumerate files, returning
    // if an error is encountered
    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        exit_status = 1;
        return 1;
    }
 
    // Allocate results for number of subdirs
    // plus 2, because we store the result for 
    // the target directory in position 0 and
    // the summary in last array element
    struct tr_args *descendents[n_subdirs+2];
    for(i=0;i<n_subdirs+2;i++) {
        descendents[i] = NULL;
    }
    pthread_t *thread_ids[max_n_threads];

    // Fill the hash table with initialization values
    // so we can identify empty slots
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    // Fill the inode lookup table with default value
    // NULL so we can identify empty slots
    for(i=0;i<INODETABLE;i++) {
        table[i] = NULL;
    }

    // Initialize thread ID pointers to NULL so we 
    // can identify unused slots
    for(i=0;i<max_n_threads;i++)
        thread_ids[i] = NULL;

    while((entry=readdir(dp))) {
        // Skip parent navigational entry
        if(strcmp("..", entry->d_name) == 0)
            continue;

        if(exit_now)
            return 1;

        // Get the file metadata
        sprintf(temppath, "%s/%s", path, entry->d_name);
        if(lstat(temppath, &meta) != 0) {
            store_error(temppath, "entry: Could not stat file");
            continue;
        }

        // Process the file or directory
        process = insert = false;
        switch(meta.st_mode & S_IFMT) {
            case S_IFLNK:
                if(verbose)
                    printf("+symlink   %s (%ld)\n", temppath, meta.st_size);
                insert = true; 
                break;
            case S_IFREG:
                if(verbose)
                    printf("+file      %s (%ld)\n", temppath, meta.st_size);
                insert = true;
                break;
            case S_IFDIR:
                if(meta.st_dev != devnum) {
		    if(verbose)
	                printf("-skip     %s on another device (%ld)\n", temppath, meta.st_size);
		}
		else {
	            if(verbose)
                        printf("+directory %s (%ld)\n", temppath, meta.st_size);
                    if(strcmp(".", entry->d_name) == 0)
                        insert = true;
                    else
                        process = true;
		}
                break;
            default:
                if(verbose)
                    printf("-skip     %s\n", temppath);
        }

        // Skip inodes that have been previously visited
        if((insert || process) && (meta.st_nlink > 1)) {
            if((i=insert_inode(meta.st_ino, table)) != 0) {
                insert = false;
		process = false;
                if(trace)
                    printf("-inode   %s inode %lu has already been counted\n", temppath, meta.st_ino);
            }
        } 

        // Update the running usage in the hash table
        if(insert) {
            // Compute size as either file size, or size of
            // blocks the file spans
            audit_size = meta.st_size;
            if(size_in_blocks)
                audit_size = meta.st_blocks*512;

            id = meta.st_gid;
            if(summarize_by_user)
                id = meta.st_uid;

            if(insert_or_update(id, audit_size, gids, sizes) != 0) {
                store_error(temppath, "entry: GID table overflowed");
                return 1;
            }
        }


        // If it is a subdirectory, we need to launch a thread to process it
        if(process) {
             init_result(&descendents[subdir_count], temppath);

             // Launch thread to walk directory
             if(verbose)
                 printf("entry: Launch a thread to process directory %d/%d: %s\n", subdir_count+1, n_subdirs, temppath);
             thread_i=tr_find_slot(thread_ids, max_n_threads); 
             thread_ids[thread_i] = malloc(sizeof(pthread_t));
             pthread_create(thread_ids[thread_i], NULL, &fts_walk, descendents[subdir_count]);
             subdir_count += 1; 
        }
    }
    free(temppath);
    free_inode_table(table);
    closedir(dp);

    // Wait for all threads to finish
    tr_finalize(thread_ids, max_n_threads);

    // If any failures, return
    if(exit_status != 0)
        return 1;

    // Add usage from the target directory to the full result
    init_result(&descendents[0], path);
    pack_result(descendents[0], gids, sizes);

    // Add summary to full result
    init_result(&descendents[n_subdirs+1], "totals");
    if((i=add_summary(descendents, n_subdirs+2, &grand_total)) != 0)
        return 1;

    // Output result
    if(json)
        output_json(descendents, n_subdirs+2, grand_total);
    else
        output_table(descendents, n_subdirs+2, grand_total);

    // Cleanup
    for(i=0;i<n_subdirs+2;i++) {
        free_result(&descendents[i]);
    }

    return 0;
}

/* SYNOPSIS
 *   Outputs usage information for the command
 * ARGUMENT
 *   None
 * RETURN
 *   Always 0
 */
int usage() {
    printf("USAGE: dug [OPTIONS] <directory>\n\n");
    printf("OPTIONS\n");
    printf("    -b  Compute apparent size (default is size of blocks occupied)\n");
    printf("    -h  Display help information\n");
    printf("    -j  Output result in JSON format (default is plain text)\n");
    printf("    -m  Maximum errors before terminating (default is 128)\n");
    printf("    -n  Output group/user names (default output uses gids/uids)\n");
    printf("    -t  Set number of threads to use (default is 4)\n");
    printf("    -u  Summarize usage by owner (default is summarize by group)\n");
    printf("    -v  Output information about each file encountered\n");
    printf("\n");
    return 0;
}

/* SYNOPSIS
 *   Entry point for command
 * ARGUMENT
 *   Standard command line input arguments
 * RETURN
 *   0 on success, error code on failure
 */
int main(int argc, char** argv) {
    int i;
    char *path;   
    char c; 

    // If run with no arguments, output usage
    if(argc < 2) 
        return usage();

    // Parse arguments
    while((c = getopt(argc, argv, "hjvnbum:t:")) != -1) {
        switch(c) {
            case 'm':
                max_errors = parse_num(optarg);
                if(max_errors < 0 || max_errors > 65535) {
                    printf("Value for -m %s was not in range [0,65535]\n", optarg);
                    return 1;
                }
                break;
            case 'v':
                verbose = true;
                break;
            case 'j':
                json = true;
                break;
            case 'n':
	        output_names = true;
	        break;
            case 'b':
                size_in_blocks = false;
                break; 
            case 'u':
                summarize_by_user = true;
                break;
            case 'h':
                usage();
                return 0;
            case 't':
                n_threads = parse_num(optarg);
                if(n_threads < 0 || n_threads > 16) {
                    printf("Value for -t %s was not in range [0,16]\n", optarg);
                    return 1;
                }
                break;
        }
    }

    // Parse path, or exit if not specified
    if (optind >= argc) {
        printf("Path argument is required! Review usage with -h\n");
        return 1;
    }
    path = argv[optind];
    if(verbose)
        printf("Auditing directory %s\n", path);

    // Initialize error string to requested
    // number of pointers
    error_strs = malloc(max_errors*sizeof(char*));

    // Compile the usage by group under path
    i = walk(path, n_threads);
    if(i > 0) {
        if(json) 
            json_output_failure();
        else {
            for(i=0;i<n_errors;i++)
                printf("error: %s\n", error_strs[i]);
        }
    }

    // Cleanup
    for(i=0;i<n_errors;i++) {
        free(error_strs[i]);
    }
    free(error_strs);

    return exit_status;
}

