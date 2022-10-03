#include<stdio.h>
#include<stdbool.h>
#include<stdlib.h>
#include<dirent.h>
#include<limits.h>
#include<errno.h>
#include<string.h>
#include<unistd.h>
#include<grp.h>
#include<fts.h>
#include<pthread.h>
#include<sys/stat.h>

#define MAXGIDS    128
#define MAXPATHLEN 4096 
#define INODETABLE 16384 

extern errno;
volatile bool exit_now = false;
bool  verbose = false, json = false, output_names = false, size_in_blocks = false;
int   max_errors = 128, n_errors = 0, n_threads = 4;
volatile int exit_status = 0;
char  **error_strs;

pthread_mutex_t error_mutex;

struct inode_entry {
    long long unsigned int num;
    void* next;
};

struct tr_args {
    char* path;
    int** n_results;
    unsigned long long **data;
};

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

int get_name(unsigned int gid, char* name) {
    struct group *grp = getgrgid(gid); 
    if(grp == NULL)
        sprintf(name, "%u", gid);
    else
        sprintf(name, "%s", grp->gr_name);
    return 0;
}

int insert_inode(long long unsigned int num, struct inode_entry* table[]) {
    int index = num % INODETABLE;
    struct inode_entry *entry = table[index];
    
    // Starting new linked list
    if(entry == NULL) {
        entry = malloc(sizeof(struct inode_entry));
        entry->num = num;
        entry->next = NULL;
	if(verbose)
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
    if(verbose)
        printf("Added LL node for inode %llu\n", num);
    return 0;
}

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

int output_json(void* results, int n_results, long long unsigned int total) {
    struct tr_args **descendents = results;
    int i, j;
    int out_dir = 0, out_size = 0;
    unsigned long long int gid, size;
    char* name_buffer = malloc(2048);
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

        printf("    \"%s\": {\n", descendents[i]->path);
        for(j=0;j<**(descendents[i]->n_results)*2;j+=2) {
            gid = (*(descendents[i]->data))[j];
            size = (*(descendents[i]->data))[j+1];
            if(output_names)
                get_name(gid, name_buffer);
            else
                sprintf(name_buffer, "%u", gid);
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
            sprintf(name_buffer, "%u", gid);
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

void init_result(struct tr_args **result, char* dir) {
    (*result) = (struct tr_args*)malloc(sizeof(struct tr_args));
    (*result)->path = malloc(strlen(dir)+1);
    sprintf((*result)->path, "%s", dir);
    (*result)->n_results = (int**)malloc(sizeof(int**));
    (*result)->data = (long long unsigned int**)malloc(sizeof(long long unsigned int**));
}

void free_result(struct tr_args **result) {
  free(*((*result)->data));
  free((*result)->data);
  free(*((*result)->n_results));
  free((*result)->n_results);
  free((*result)->path);
  free(*result);
}

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

int add_summary(void* tstructs, int n_results, long long unsigned int *total) {
    struct tr_args **results = tstructs;
    int i, j;
    int out_dir = 0, out_size = 0;
    unsigned long long int size, sizes[MAXGIDS];
    unsigned int gid, gids[MAXGIDS];

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

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

static void* fts_walk(void *arg) {
    FTS *stream;
    FTSENT *entry;
    int i, j;
    bool insert = false;
    bool error = false;
    long long unsigned int audit_size;
    long long unsigned int sizes[MAXGIDS];
    unsigned int gids[MAXGIDS];
    struct inode_entry *table[INODETABLE];
    struct tr_args *targs = arg;
    int** n_results = targs->n_results;
    long long unsigned int **data = targs->data;
    char* path = targs->path;

    // FTS needs a null-terminated list of paths as argument
    char *paths[2] = {path, NULL};
    stream = fts_open(paths, FTS_PHYSICAL|FTS_NOCHDIR, NULL);
    if(stream == NULL) {
        store_error(paths[0], strerror(errno));
        return "FTSOPENFAIL"; 
    }

    // Fil the group/usage hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    // Fill the inode lookup table with default values
    for(i=0;i<INODETABLE;i++) {
        table[i] = NULL;
    }

    while((entry=fts_read(stream))) {
        if(entry == NULL) {
            store_error("fts_error", strerror(errno));
            continue;
        }

        if(exit_now)
            return "TASKEXIT";

        // Process the file or directory
        insert = false;
        error = false;
        switch(entry->fts_info) {
            case FTS_F:
                if(verbose)
                    printf("+file      %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
		insert = true;
                break;
            case FTS_D:
                if(verbose)
                    printf("+directory %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_SL:
                if(verbose)
                    printf("+symink    %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_SLNONE:
                if(verbose)
                    printf("+brksymink %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_DEFAULT:
                if(verbose)
                    printf("+uncat     %s (%llu)\n", entry->fts_path, entry->fts_statp->st_size);
                insert = true;
                break;
            case FTS_DNR:
                error = true;
                break;
            case FTS_DP:
                // We already saw this directory in preorder FTS_D,
                // so ignore it
                break;
            case FTS_NS:
                if(verbose)
                    printf("-stat_err  %s %s\n", entry->fts_path, strerror(errno));
                error = true;
                break;
            case FTS_ERR:
                if(verbose)
                    printf("-fts_err   %s\n", entry->fts_path);
                error = true;
                break;
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
	        if(verbose)
	            printf("-inode   %s inode %llu has already been counted\n", entry->fts_path, entry->fts_statp->st_ino);
	    }
	}

        // Update the running usage in the hash table
        if(insert) {
            // Compute size as either file size, or size of
            // blocks the file spans
            audit_size = entry->fts_statp->st_size;
            if(size_in_blocks)
                audit_size = entry->fts_statp->st_blocks*512;
 
            if(insert_or_update(entry->fts_statp->st_gid, audit_size, gids, sizes) != 0) {
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

int get_n_subdirs(char* path, unsigned int *n_subdirs) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    int i, status;
    char* temppath = malloc(MAXPATHLEN);
    unsigned int sd = 0;

    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        return 1;
    }

    while((entry=readdir(dp))) {
        if(strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0)
            continue;

        sprintf(temppath, "%s/%s", path, entry->d_name);
        if(lstat(temppath, &meta) != 0) {
            store_error(temppath, "Could not stat file");
            continue;
        }

        switch(meta.st_mode & S_IFMT) {
            case S_IFDIR:
                sd += 1;
        }
    }
 
    *n_subdirs = sd;
    closedir(dp);
    free(temppath);
    return 0;
}

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

int tr_find_slot(pthread_t *thread_ids[], unsigned int max_n_threads) {
    int i, status;
    int slot = -1;
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

int tr_finalize(pthread_t *thread_ids[], unsigned int max_n_threads) {
    int i, status, n=-1;
    for(i=0;i<max_n_threads;i++) {
        if(thread_ids[i] == NULL)
            continue;

        status = pthread_join(*thread_ids[i], NULL);
        if(status != 0)
            printf("tr   :Error in pthread_join(): %s\n", strerror(errno));
        free(thread_ids[i]);
    }
    return n;
}

int walk(char* path, unsigned int max_n_threads) {
    DIR *dp;
    struct dirent *entry;
    struct stat meta;
    int i, j, status, thread_i;
    char* temppath = malloc(MAXPATHLEN);
    bool insert, process;
    long long unsigned int audit_size, grand_total=0;
    long long unsigned int sizes[MAXGIDS];
    long long unsigned int **data;
    unsigned int gids[MAXGIDS];
    unsigned int n_subdirs = 0, subdir_count=1, n_groups = 0;
    struct inode_entry *table[INODETABLE];

    if((status=get_n_subdirs(path, &n_subdirs)) != 0) {
        exit_status = 1;
        return 1;
    }
 
    // Allocate results for number of subdirs
    // plus 1, because we store the result for ./
    // in position 0
    struct tr_args *descendents[n_subdirs+1];
    for(i=0;i<n_subdirs;i++) {
        descendents[i] = NULL;
    }
    pthread_t *thread_ids[max_n_threads];


    dp = opendir(path);
    if(dp == NULL) {
        store_error(path, strerror(errno));
        free(temppath);
        exit_status = 1;
        return 1;
    }

    // Fil the hash table with initialization values
    for(i=0;i<MAXGIDS;i++) {
        gids[i] = UINT_MAX;
        sizes[i] = 0;
    }

    // Fill the inode lookup table with default values
    for(i=0;i<INODETABLE;i++) {
        table[i] = NULL;
    }

    // Fill thread ID pointers with NULL
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
                    printf("entry: Not following symlink %s\n", temppath);
                insert = true; 
                break;
            case S_IFREG:
                if(verbose)
                    printf("entry: Processing regular file %s\n", temppath);
                insert = true;
                break;
            case S_IFDIR:
                if(verbose)
                    printf("entry: Processing directory %s\n", temppath);
                if(strcmp(".", entry->d_name) == 0)
                    insert = true;
                else
                    process = true;
                break;
            default:
                if(verbose)
                    printf("entry: Skipping file %s\n", temppath);
        }

        // Skip inodes that have been previously visited
        if((insert || process) && (meta.st_nlink > 1)) {
            if((i=insert_inode(meta.st_ino, table)) != 0) {
                insert = false;
		process = false;
                if(verbose)
                    printf("-inode   %s inode %llu has already been counted\n", temppath, meta.st_ino);
            }
        } 

        // Update the running usage in the hash table
        if(insert) {
            // Compute size as either file size, or size of
            // blocks the file spans
            audit_size = meta.st_size;
            if(size_in_blocks)
                audit_size = meta.st_blocks*512;

            if(insert_or_update(meta.st_gid, audit_size, gids, sizes) != 0) {
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

    // Add summary
    init_result(&descendents[n_subdirs+1], "totals");
    if((i=add_summary(descendents, n_subdirs+2, &grand_total)) != 0)
        return 1;

    // Output result
    output_json(descendents, n_subdirs+2, grand_total);

    // Cleanup
    for(i=0;i<n_subdirs+2;i++) {
        free_result(&descendents[i]);
    }

    return 0;
}

int usage() {
    printf("USAGE: dug [OPTIONS] <directory>\n\n");
    printf("OPTIONS\n");
    printf("    -b  Compute size of blocks occupied (default is file sizes)\n");
    printf("    -h  Display help information\n");
    printf("    -j  Output result in JSON format (default is plain text)\n");
    printf("    -m  Maximum errors before terminating (default is 128)\n");
    printf("    -n  Output group names (default output uses gids)\n");
    printf("    -t  Set number of threads to use (default is 4)\n");
    printf("    -v  Output information about each file encountered\n");
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    int i;
    char *path;   
    char c; 

    // If run with no arguments, output usage
    if(argc < 2) 
        return usage();

    // Parse arguments
    while((c = getopt(argc, argv, "hjvnbm:t:")) != -1) {
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
                size_in_blocks = true;
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

    // Parse path, or exit if non specified
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
        for(i=0;i<n_errors;i++)
            printf("error: %s\n", error_strs[i]);
        if(exit_status == 1)
            printf("The argument directory %s could not be opened\n", path);
        else if(exit_status == 2) 
            printf("The number of groups encountered exceeded the maximum value of %u\n", MAXGIDS);
        else if(exit_status == 3)
            printf("The maximum number of errors %d was exceeded\n", max_errors);
    }

    // Cleanup
    for(i=0;i<n_errors;i++) {
        free(error_strs[i]);
    }
    free(error_strs);

    return exit_status;
}

